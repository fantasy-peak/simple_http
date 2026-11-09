#pragma once

// Typed request extractors (axum `FromRequest` / gin `ShouldBind*`, but
// compile-time typed) -- C++17 token-only port.
//
// An extractor declares one value a handler needs (path parameters, query,
// JSON body, form body, application state, a header, a cookie) and knows how to
// parse it and how to reject it. `extract<...>(handler)` wraps an ordinary
// callback handler: the extractors run before it, the first failure is answered
// as a 400/422/500 and the handler is never entered. The wrapper is still a
// normal Handler, so Router::route, middleware, groups and OpenAPI descriptors
// are unchanged.
//
//     server.route({Method::Post}, "/users/{id}",
//         simple_http::extract<simple_http::Path<UserParams>, simple_http::Json<CreateUser>>(
//             [](const UserParams &p, CreateUser in, RequestPtr req, ResponsePtr res,
//                HandlerCompletion done) {
//                 res->status(simple_http::status::created);
//                 res->async_send("ok", simple_http::finish_with(std::move(done)));
//             }));
//
// Extracted values are passed first, then the Request and Response (and, for a
// callback handler, the completion handler last). A trailing SslHandle is
// supported as well. Exactly one body extractor (Json/Form) may appear: the
// body is a stream and can be consumed only once. Use Valid<E> around an
// extractor to run Validator<T> (see validation.h).
//
// The Json/Form extractors depend on glaze and are therefore only available
// when SIMPLE_HTTP_HAS_GLAZE is set (see core/glaze_support.h). Path,
// Query, State, Header, Cookie and Valid<...> work in a plain C++17 build;
// Path/Query themselves also use glaze reflection, so they are gated too.

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../core/glaze_support.h"
#include "../core/logging.h"
#include "../core/result.h"
#include "../core/types.h"
#include "../proto/error_body.h"
#include "../proto/form.h"
#include "../proto/json.h"
#include "../proto/params.h"
#include "../proto/request.h"
#include "../proto/response.h"
#include "handler.h"
#include "validation.h"

namespace simple_http {

// A failure an extractor decides on, rendered by render_http_error() unless the
// caller installs its own wrapper. `status` is the HTTP status, `code`/`message`
// populate the JsonError body, and `details` is an optional JSON fragment (the
// validation extractor puts the field-error array there). status == 0 is the
// internal "success" marker used between the extraction steps.
struct HttpError {
    int status{simple_http::status::bad_request};
    std::string code;
    std::string message;
    std::string details;
};

namespace detail {
inline HttpError extract_ok() {
    return HttpError{0, {}, {}, {}};
}
} // namespace detail

// A body read can fail because it was malformed/truncated (400) or because the
// decoded representation exceeded the configured limit (413). Everything else
// stays a bad request; the handler can inspect the original error if it needs
// to distinguish further.
inline int body_error_status(const error_code &ec) {
    return ec == asio::error::message_size ? status::payload_too_large : status::bad_request;
}

// Renders an HttpError as the library's standard JSON error body and completes
// `done` when the response has been written.
inline void render_http_error(const std::shared_ptr<ResponseWriter> &res, const HttpError &error,
                              HandlerCompletion done) {
    JsonError body{.code = error.code.empty() ? std::string{"error"} : error.code,
                   .message = error.message.empty() ? std::string{"request failed"} : error.message};
    if (!error.details.empty()) {
        body.details = error.details;
    }
    async_write_error(res, error.status, std::move(body), finish_with(std::move(done)));
}

// A structural string usable as a non-type template parameter, so a header or
// cookie extractor can carry its name in the type:
//     extract<Header<int, "x-page">, Cookie<std::string, "sid">>(...)
//
// Class-type non-type template parameters (the mechanism that lets a string
// literal name a type) need C++20. A plain C++17 build does not get the
// Header/Cookie extractors; every other extractor works.
#if defined(__cplusplus) && __cplusplus >= 202002L
template <std::size_t N> struct FixedString {
    std::array<char, N> value{};
    constexpr FixedString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = s[i];
        }
    }
    [[nodiscard]] constexpr std::string_view view() const { return {value.data(), N - 1}; }
};
template <std::size_t N> FixedString(const char (&)[N]) -> FixedString<N>;
#endif // C++20 class-type NTTP

// --- extractor declarations -------------------------------------------------

template <typename T> struct Path {
    using value_type = T;
};
template <typename T> struct Query {
    using value_type = T;
};
template <typename T> struct Json {
    using value_type = T;
};
template <typename T> struct Form {
    using value_type = T;
};
template <typename T> struct State {
    using value_type = T *;
};
#if defined(__cplusplus) && __cplusplus >= 202002L
template <typename T, FixedString Name> struct Header {
    using value_type = T;
    static constexpr std::string_view name = Name.view();
};
template <typename T, FixedString Name> struct Cookie {
    using value_type = T;
    static constexpr std::string_view name = Name.view();
};
#endif
// Runs Validator<T> after the inner extractor succeeds. Use it around the
// extractor that produced the value: Valid<Json<CreateUser>>.
template <typename E> struct Valid {
    using inner = E;
};

template <typename E> struct Extractor {
    static_assert(sizeof(E) == 0, "no extractor is defined for this type");
};

// --- extractor implementations ----------------------------------------------
//
// Every extractor implements:
//     static void async_run(Request &, std::function<void(HttpError, T)>);
// A completion with HttpError::status == 0 is success.

#if SIMPLE_HTTP_HAS_GLAZE
template <typename T> struct Extractor<Path<T>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        auto value = path_params<T>(req);
        if (!value) {
            done(HttpError{status::bad_request, "invalid_path", "invalid or missing path parameters", {}}, T{});
            return;
        }
        done(detail::extract_ok(), std::move(*value));
    }
};

template <typename T> struct Extractor<Query<T>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        auto value = query_params<T>(req);
        if (!value) {
            done(HttpError{status::bad_request, "invalid_query", "invalid or missing query parameters", {}}, T{});
            return;
        }
        done(detail::extract_ok(), std::move(*value));
    }
};

template <typename T> struct Extractor<Json<T>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        async_read_json_body<T>(
            req, asio::bind_cancellation_slot(req.cancellation_slot(),
                                              [done = std::move(done)](error_code ec, result<T> value) mutable {
                                                  if (!value) {
                                                      const error_code inner = value ? error_code{} : value.error();
                                                      (void)ec;
                                                      done(HttpError{body_error_status(inner), "invalid_json",
                                                                     "malformed JSON request body", inner.message()},
                                                           T{});
                                                      return;
                                                  }
                                                  (void)ec;
                                                  done(detail::extract_ok(), std::move(*value));
                                              }));
    }
};

template <typename T> struct Extractor<Form<T>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        read_urlencoded_body(
            req, asio::bind_cancellation_slot(
                     req.cancellation_slot(), [done = std::move(done)](error_code ec, QueryParams form) mutable {
                         if (ec) {
                             done(HttpError{body_error_status(ec), "invalid_form", "malformed URL-encoded request body",
                                            ec.message()},
                                  T{});
                             return;
                         }
                         auto value = parse_params<T>(form);
                         if (!value) {
                             done(HttpError{status::bad_request, "invalid_form", "invalid form field types", {}}, T{});
                             return;
                         }
                         done(detail::extract_ok(), std::move(*value));
                     }));
    }
};
#endif // SIMPLE_HTTP_HAS_GLAZE

template <typename T> struct Extractor<State<T>> {
    using value_type = T *;

    static void async_run(Request &req, std::function<void(HttpError, T *)> done) {
        T *state = req.app_state<T>();
        if (!state) {
            done(HttpError{status::internal_server_error,
                           "state_missing",
                           "application state is not configured for this route",
                           {}},
                 nullptr);
            return;
        }
        done(detail::extract_ok(), state);
    }
};

namespace detail {

// A header/cookie value is a plain string; numeric and boolean types are parsed
// as their JSON scalar spelling. Strings are taken verbatim (they are not
// JSON-quoted on the wire).
template <typename T> std::optional<T> parse_scalar(std::string_view value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return std::string{value};
    } else {
#if SIMPLE_HTTP_HAS_GLAZE
        T out{};
        if (auto ec = glz::read_json(out, value); ec) {
            return std::nullopt;
        }
        return out;
#else
        static_assert(std::is_same_v<T, std::string>,
                      "non-string Header/Cookie extractors require the glaze feature (C++23 build)");
        return std::nullopt;
#endif
    }
}

} // namespace detail

#if defined(__cplusplus) && __cplusplus >= 202002L
template <typename T, FixedString Name> struct Extractor<Header<T, Name>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        const auto value = req.header(Name.view());
        if (!value) {
            done(HttpError{status::bad_request, "missing_header", "missing required header", std::string{Name.view()}},
                 T{});
            return;
        }
        auto parsed = detail::parse_scalar<T>(*value);
        if (!parsed) {
            done(HttpError{status::bad_request, "invalid_header", "invalid header value", std::string{Name.view()}},
                 T{});
            return;
        }
        done(detail::extract_ok(), std::move(*parsed));
    }
};

template <typename T, FixedString Name> struct Extractor<Cookie<T, Name>> {
    using value_type = T;

    static void async_run(Request &req, std::function<void(HttpError, T)> done) {
        const auto value = req.cookie(Name.view());
        if (!value) {
            done(HttpError{status::bad_request, "missing_cookie", "missing required cookie", std::string{Name.view()}},
                 T{});
            return;
        }
        auto parsed = detail::parse_scalar<T>(*value);
        if (!parsed) {
            done(HttpError{status::bad_request, "invalid_cookie", "invalid cookie value", std::string{Name.view()}},
                 T{});
            return;
        }
        done(detail::extract_ok(), std::move(*parsed));
    }
};
#endif // C++20 class-type NTTP

template <typename E> struct Extractor<Valid<E>> {
    using value_type = typename Extractor<E>::value_type;

    static void async_run(Request &req, std::function<void(HttpError, value_type)> done) {
        Extractor<E>::async_run(req, [done = std::move(done)](HttpError error, value_type value) mutable {
            if (error.status != 0) {
                done(std::move(error), value_type{});
                return;
            }
            auto valid = Validator<value_type>::validate(value);
            if (!valid) {
                done(HttpError{status::unprocessable_content, "validation_failed", "request validation failed",
                               field_errors_json(valid.error())},
                     value_type{});
                return;
            }
            done(detail::extract_ok(), std::move(value));
        });
    }
};

// --- extraction driver ------------------------------------------------------

namespace detail {

template <typename E> struct is_body_extractor : std::false_type {};
template <typename T> struct is_body_extractor<Json<T>> : std::true_type {};
template <typename T> struct is_body_extractor<Form<T>> : std::true_type {};
template <typename E> struct is_body_extractor<Valid<E>> : is_body_extractor<E> {};

template <std::size_t I, typename... Es> struct type_at;
template <typename E, typename... Rest> struct type_at<0, E, Rest...> {
    using type = E;
};
template <std::size_t I, typename E, typename... Rest> struct type_at<I, E, Rest...> : type_at<I - 1, Rest...> {};

template <typename... Es> using extracted_values = std::tuple<typename Extractor<Es>::value_type...>;

template <typename... Es> struct ExtractionContext {
    explicit ExtractionContext(Request &request) : req(request) {}

    Request &req;
    std::tuple<std::optional<typename Extractor<Es>::value_type>...> values;
    std::function<void(HttpError)> on_error;
    std::function<void(extracted_values<Es...>)> on_success;
};

template <typename... Es, std::size_t... Is>
extracted_values<Es...> collect_values(ExtractionContext<Es...> &ctx, std::index_sequence<Is...>) {
    return extracted_values<Es...>{std::move(*std::get<Is>(ctx.values))...};
}

template <std::size_t I, typename... Es> void extract_step(const std::shared_ptr<ExtractionContext<Es...>> &ctx) {
    if constexpr (I == sizeof...(Es)) {
        ctx->on_success(collect_values(*ctx, std::index_sequence_for<Es...>{}));
    } else {
        using E = typename type_at<I, Es...>::type;
        using T = typename Extractor<E>::value_type;
        Extractor<E>::async_run(ctx->req, [ctx](HttpError error, T value) {
            if (error.status != 0) {
                ctx->on_error(std::move(error));
                return;
            }
            std::get<I>(ctx->values) = std::move(value);
            extract_step<I + 1, Es...>(ctx);
        });
    }
}

// --- calling the wrapped handler with the extracted values ------------------

template <typename F, typename Tuple, typename = void> struct extracted_callback_ssl : std::false_type {};
template <typename F, typename... Vs>
struct extracted_callback_ssl<
    F, std::tuple<Vs...>,
    std::enable_if_t<std::is_invocable_v<F &, Vs..., RequestPtr, ResponsePtr, SslHandle, HandlerCompletion>>>
    : std::true_type {};

template <typename F, typename Tuple, typename = void> struct extracted_callback : std::false_type {};
template <typename F, typename... Vs>
struct extracted_callback<F, std::tuple<Vs...>,
                          std::enable_if_t<std::is_invocable_v<F &, Vs..., RequestPtr, ResponsePtr, HandlerCompletion>>>
    : std::true_type {};

template <typename F, typename Tuple, typename = void> struct extracted_sync_ssl : std::false_type {};
template <typename F, typename... Vs>
struct extracted_sync_ssl<
    F, std::tuple<Vs...>,
    std::enable_if_t<std::is_same_v<std::invoke_result_t<F &, Vs..., RequestPtr, ResponsePtr, SslHandle>, void>>>
    : std::true_type {};

template <typename F, typename Tuple, typename = void> struct extracted_sync : std::false_type {};
template <typename F, typename... Vs>
struct extracted_sync<F, std::tuple<Vs...>,
                      std::enable_if_t<std::is_same_v<std::invoke_result_t<F &, Vs..., RequestPtr, ResponsePtr>, void>>>
    : std::true_type {};

template <typename F, typename Tuple>
void invoke_extracted(std::shared_ptr<F> fn, Tuple values, RequestPtr req, ResponsePtr res, SslHandle ssl,
                      HandlerCompletion done) {
    using D = F;
    if constexpr (extracted_callback_ssl<D, Tuple>::value) {
        std::apply(
            [&](auto &&...args) {
                (*fn)(std::forward<decltype(args)>(args)..., std::move(req), std::move(res), ssl, std::move(done));
            },
            std::move(values));
    } else if constexpr (extracted_callback<D, Tuple>::value) {
        std::apply(
            [&](auto &&...args) {
                (*fn)(std::forward<decltype(args)>(args)..., std::move(req), std::move(res), std::move(done));
            },
            std::move(values));
    } else if constexpr (extracted_sync_ssl<D, Tuple>::value) {
        std::apply(
            [&](auto &&...args) { (*fn)(std::forward<decltype(args)>(args)..., std::move(req), std::move(res), ssl); },
            std::move(values));
        std::move(done)();
    } else if constexpr (extracted_sync<D, Tuple>::value) {
        std::apply(
            [&](auto &&...args) { (*fn)(std::forward<decltype(args)>(args)..., std::move(req), std::move(res)); },
            std::move(values));
        std::move(done)();
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
    } else if constexpr (detail::extracted_coro_ssl<D, Tuple>::value) {
        detail::execute_extracted_coroutine(req, res, std::move(done), fn,
                                            std::tuple_cat(std::move(values), std::make_tuple(req, res, ssl)));
    } else if constexpr (detail::extracted_coro<D, Tuple>::value) {
        detail::execute_extracted_coroutine(req, res, std::move(done), fn,
                                            std::tuple_cat(std::move(values), std::make_tuple(req, res)));
#endif
    } else {
        static_assert(sizeof(F) == 0,
                      "extract handler must be callable as (extracted..., RequestPtr, ResponsePtr[, SslHandle]"
                      "[, HandlerCompletion]) or return a coroutine");
    }
}

} // namespace detail

// Runs every extractor, short-circuiting on the first failure, and completes
// with the tuple of values. Completion signature: void(HttpError, Tuple). This
// is the tuple-only form of extract(); it is useful when a caller wants to
// inspect the extracted values before choosing a handler.
namespace detail {

template <typename Handler, typename... Es> struct ExtractAllState : ExtractionContext<Es...> {
    ExtractAllState(Request &request, Handler handler)
        : ExtractionContext<Es...>(request), completion(std::move(handler)) {}

    using Values = extracted_values<Es...>;
    Handler completion;
};

} // namespace detail

template <typename... Es, typename Token> auto async_extract_all(Request &req, Token &&token) {
    return asio::async_initiate<Token, void(HttpError, detail::extracted_values<Es...>)>(
        [&req](auto handler) {
            using Handler = std::decay_t<decltype(handler)>;
            using State = detail::ExtractAllState<Handler, Es...>;
            auto state = std::make_shared<State>(req, std::move(handler));
            // The extraction callbacks below capture a raw pointer: the context
            // owns them, and every callback runs while the extraction chain
            // still holds the context's shared_ptr (see extract_step), so the
            // state cannot be destroyed mid-callback. Capturing the shared_ptr
            // here would be a ownership cycle (state -> context -> callback).
            State *raw = state.get();
            raw->on_error = [raw](HttpError error) mutable {
                std::move(raw->completion)(std::move(error), typename State::Values{});
            };
            raw->on_success = [raw](typename State::Values values) mutable {
                std::move(raw->completion)(detail::extract_ok(), std::move(values));
            };
            // Explicit upcast: GCC does not apply the shared_ptr converting
            // constructor to an explicitly-specialized function-template
            // parameter.
            std::shared_ptr<detail::ExtractionContext<Es...>> context = state;
            detail::extract_step<0, Es...>(context);
        },
        token);
}

// Wraps a handler with extractors. The handler receives the extracted values
// first, then Request and Response (and optionally SslHandle; callback handlers
// take HandlerCompletion last).
template <typename... Es, typename F> Handler extract(F &&fn) {
    static_assert((std::size_t{0} + ... + (detail::is_body_extractor<Es>::value ? 1 : 0)) <= 1,
                  "at most one body extractor (Json/Form) may be used in one extract<...>");
    using values = detail::extracted_values<Es...>;
    using Fn = std::decay_t<F>;
    static_assert(detail::extracted_callback_ssl<Fn, values>::value || detail::extracted_callback<Fn, values>::value ||
                      detail::extracted_sync_ssl<Fn, values>::value || detail::extracted_sync<Fn, values>::value
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
                      || detail::extracted_coro_ssl<Fn, values>::value || detail::extracted_coro<Fn, values>::value
#endif
                  ,
                  "extract handler must be callable as (extracted..., RequestPtr, ResponsePtr[, SslHandle])");

    auto function = std::make_shared<Fn>(std::forward<F>(fn));
    return make_handler([function](RequestPtr req, ResponsePtr res, SslHandle ssl, HandlerCompletion done) mutable {
        using Context = detail::ExtractionContext<Es...>;
        auto context = std::make_shared<Context>(*req);
        // HandlerCompletion is move-only, while std::function (the shape the
        // extraction context stores) requires copy-constructible targets. Hold
        // the completion in a shared_ptr and move out of it at the single
        // terminal call; each of on_error/on_success runs at most once.
        auto completion = std::make_shared<HandlerCompletion>(std::move(done));
        context->on_error = [res, completion](HttpError error) mutable {
            render_http_error(res, error, std::move(*completion));
        };
        // on_success must not capture `context` by shared_ptr: the context owns
        // on_success, so that would be a reference cycle. The extraction chain
        // keeps the context alive until on_success returns.
        context->on_success = [function, req, res, ssl, completion](values extracted) mutable {
            detail::invoke_extracted(function, std::move(extracted), std::move(req), std::move(res), ssl,
                                     std::move(*completion));
        };
        detail::extract_step<0, Es...>(context);
    });
}

} // namespace simple_http
