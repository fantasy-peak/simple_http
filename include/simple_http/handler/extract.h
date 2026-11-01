#pragma once

// Typed request extractors (axum `FromRequest` / gin `ShouldBind*`, but
// compile-time typed).
//
// An extractor declares one value a handler needs (path parameters, query,
// JSON body, form body, application state) and knows how to parse it and how to
// reject it. `extract<...>(handler)` wraps an ordinary coroutine handler: the
// extractors run before it, the first failure is answered as a 400/422/500 and
// the handler is never entered. The wrapper still returns a normal Handler, so
// Router::route, middleware, groups and OpenAPI descriptors are unchanged.
//
//     server.route({Method::Post}, "/users/{id}",
//         extract<Path<UserParams>, Json<CreateUser>>(
//             [](const UserParams &p, CreateUser in, RequestPtr req, ResponsePtr res)
//                 -> asio::awaitable<void> {
//                 co_await res->status(201).send("ok");
//             }));
//
// Extracted values are passed first, then the Request and Response. A handler
// that takes a trailing SslHandle is supported as well. Exactly one body
// extractor (Json/Form) may appear: the body is a stream and can be consumed
// only once. Use Valid<E> around an extractor to run Validator<T> (see
// validation.h).

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../proto/error_body.h"
#include "../proto/form.h"
#include "../proto/json.h"
#include "../proto/params.h"
#include "handler.h"
#include "validation.h"

namespace simple_http {

// A failure an extractor decides on, rendered by render_http_error() unless the
// caller installs its own wrapper. `status` is the HTTP status, `code`/`message`
// populate the JsonError body, and `details` is an optional JSON fragment (the
// validation extractor puts the field-error array there).
struct HttpError {
    int status{400};
    std::string code;
    std::string message;
    std::string details;
};

// Renders an HttpError as the library's standard JSON error body.
inline asio::awaitable<void> render_http_error(ResponsePtr res, const HttpError &error) {
    JsonError body{.code = error.code.empty() ? std::string{"error"} : error.code,
                   .message = error.message.empty() ? std::string{"request failed"} : error.message};
    if (!error.details.empty()) {
        body.details = error.details;
    }
    (void)co_await write_error(res, error.status, std::move(body));
}

// A structural string usable as a non-type template parameter, so a header or
// cookie extractor can carry its name in the type:
//     extract<Header<int, "x-page">, Cookie<std::string, "sid">>(...)
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
template <typename T, FixedString Name> struct Header {
    using value_type = T;
    static constexpr std::string_view name = Name.view();
};
template <typename T, FixedString Name> struct Cookie {
    using value_type = T;
    static constexpr std::string_view name = Name.view();
};
// Runs Validator<T> after the inner extractor succeeds. Use it around the
// extractor that produced the value: Valid<Json<CreateUser>>.
template <typename E> struct Valid {
    using inner = E;
};

template <typename E> struct Extractor {
    static_assert(sizeof(E) == 0, "no extractor is defined for this type");
};

// --- extractor implementations ----------------------------------------------

template <typename T> struct Extractor<Path<T>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        auto value = path_params<T>(req);
        if (!value) {
            co_return std::unexpected(HttpError{400, "invalid_path", "invalid or missing path parameters", {}});
        }
        co_return std::move(*value);
    }
};

template <typename T> struct Extractor<Query<T>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        auto value = query_params<T>(req);
        if (!value) {
            co_return std::unexpected(HttpError{400, "invalid_query", "invalid or missing query parameters", {}});
        }
        co_return std::move(*value);
    }
};

template <typename T> struct Extractor<Json<T>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        auto value = co_await read_json_body<T>(req);
        if (!value) {
            co_return std::unexpected(
                HttpError{400, "invalid_json", "malformed JSON request body", value.error().message()});
        }
        co_return std::move(*value);
    }
};

template <typename T> struct Extractor<Form<T>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        auto form = co_await read_urlencoded_body(req);
        if (!form) {
            co_return std::unexpected(
                HttpError{400, "invalid_form", "malformed URL-encoded request body", form.error().message()});
        }
        auto value = parse_params<T>(*form);
        if (!value) {
            co_return std::unexpected(HttpError{400, "invalid_form", "invalid form field types", {}});
        }
        co_return std::move(*value);
    }
};

template <typename T> struct Extractor<State<T>> {
    using value_type = T *;

    static asio::awaitable<std::expected<T *, HttpError>> run(Request &req) {
        T *state = req.app_state<T>();
        if (!state) {
            co_return std::unexpected(
                HttpError{500, "state_missing", "application state is not configured for this route", {}});
        }
        co_return state;
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
        T out{};
        if (auto ec = glz::read_json(out, value); ec) {
            return std::nullopt;
        }
        return out;
    }
}

} // namespace detail

template <typename T, FixedString Name> struct Extractor<Header<T, Name>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        const auto value = req.header(Name.view());
        if (!value) {
            co_return std::unexpected(
                HttpError{400, "missing_header", "missing required header", std::string{Name.view()}});
        }
        auto parsed = detail::parse_scalar<T>(*value);
        if (!parsed) {
            co_return std::unexpected(
                HttpError{400, "invalid_header", "invalid header value", std::string{Name.view()}});
        }
        co_return std::move(*parsed);
    }
};

template <typename T, FixedString Name> struct Extractor<Cookie<T, Name>> {
    using value_type = T;

    static asio::awaitable<std::expected<T, HttpError>> run(Request &req) {
        const auto value = req.cookie(Name.view());
        if (!value) {
            co_return std::unexpected(
                HttpError{400, "missing_cookie", "missing required cookie", std::string{Name.view()}});
        }
        auto parsed = detail::parse_scalar<T>(*value);
        if (!parsed) {
            co_return std::unexpected(
                HttpError{400, "invalid_cookie", "invalid cookie value", std::string{Name.view()}});
        }
        co_return std::move(*parsed);
    }
};

template <typename E> struct Extractor<Valid<E>> {
    using value_type = typename Extractor<E>::value_type;

    static asio::awaitable<std::expected<value_type, HttpError>> run(Request &req) {
        auto value = co_await Extractor<E>::run(req);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto valid = Validator<value_type>::validate(*value);
        if (!valid) {
            co_return std::unexpected(
                HttpError{422, "validation_failed", "request validation failed", field_errors_json(valid.error())});
        }
        co_return std::move(*value);
    }
};

// --- extraction driver ------------------------------------------------------

namespace detail {

template <typename E> struct is_body_extractor : std::false_type {};
template <typename T> struct is_body_extractor<Json<T>> : std::true_type {};
template <typename T> struct is_body_extractor<Form<T>> : std::true_type {};
template <typename E> struct is_body_extractor<Valid<E>> : is_body_extractor<E> {};

template <typename... Es> struct ExtractorRunner;

template <> struct ExtractorRunner<> {
    static asio::awaitable<std::expected<std::tuple<>, HttpError>> run(Request &) { co_return std::tuple<>{}; }
};

template <typename E, typename... Rest> struct ExtractorRunner<E, Rest...> {
    using first_type = typename Extractor<E>::value_type;
    using result_type = std::tuple<first_type, typename Extractor<Rest>::value_type...>;

    static asio::awaitable<std::expected<result_type, HttpError>> run(Request &req) {
        auto first = co_await Extractor<E>::run(req);
        if (!first) {
            co_return std::unexpected(first.error());
        }
        auto rest = co_await ExtractorRunner<Rest...>::run(req);
        if (!rest) {
            co_return std::unexpected(rest.error());
        }
        co_return std::tuple_cat(std::make_tuple(std::move(*first)), std::move(*rest));
    }
};

template <typename F, typename Tuple, typename = void> struct accepts_ssl : std::false_type {};

template <typename F, typename... Args>
struct accepts_ssl<F, std::tuple<Args...>,
                   std::void_t<decltype(std::declval<F &>()(std::declval<Args>()..., std::declval<RequestPtr>(),
                                                            std::declval<ResponsePtr>(), std::declval<SslHandle>()))>>
    : std::true_type {};

template <typename F, typename Tuple, typename = void> struct accepts_plain : std::false_type {};

template <typename F, typename... Args>
struct accepts_plain<F, std::tuple<Args...>,
                     std::void_t<decltype(std::declval<F &>()(std::declval<Args>()..., std::declval<RequestPtr>(),
                                                              std::declval<ResponsePtr>()))>> : std::true_type {};

template <typename... Es> using extractor_values = std::tuple<typename Extractor<Es>::value_type...>;

} // namespace detail

// Runs every extractor, short-circuiting on the first failure. Exposed for
// callers that want the tuple without the handler wrapper.
template <typename... Es>
asio::awaitable<std::expected<detail::extractor_values<Es...>, HttpError>> extract_all(Request &req) {
    co_return co_await detail::ExtractorRunner<Es...>::run(req);
}

// Wraps a coroutine handler with extractors. The handler receives the extracted
// values first, then Request and Response (and optionally SslHandle).
template <typename... Es, typename F> Handler extract(F &&fn) {
    static_assert((std::size_t{0} + ... + (detail::is_body_extractor<Es>::value ? 1 : 0)) <= 1,
                  "at most one body extractor (Json/Form) may be used in one extract<...>");
    using values = detail::extractor_values<Es...>;
    using Fn = std::decay_t<F>;
    static_assert(detail::accepts_ssl<Fn, values>::value || detail::accepts_plain<Fn, values>::value,
                  "extract handler must be callable as (extracted..., RequestPtr, ResponsePtr[, SslHandle])");

    if constexpr (detail::accepts_ssl<Fn, values>::value) {
        auto wrapper = [fn = Fn(std::forward<F>(fn))](RequestPtr req, ResponsePtr res,
                                                      SslHandle ssl) mutable -> asio::awaitable<void> {
            auto values_result = co_await detail::ExtractorRunner<Es...>::run(*req);
            if (!values_result) {
                co_await render_http_error(std::move(res), values_result.error());
                co_return;
            }
            co_await std::apply(
                [&](auto &&...args) -> asio::awaitable<void> {
                    co_await fn(std::forward<decltype(args)>(args)..., std::move(req), std::move(res), ssl);
                },
                std::move(*values_result));
        };
        return make_handler(std::move(wrapper));
    } else {
        auto wrapper = [fn = Fn(std::forward<F>(fn))](RequestPtr req,
                                                      ResponsePtr res) mutable -> asio::awaitable<void> {
            auto values_result = co_await detail::ExtractorRunner<Es...>::run(*req);
            if (!values_result) {
                co_await render_http_error(std::move(res), values_result.error());
                co_return;
            }
            co_await std::apply(
                [&](auto &&...args) -> asio::awaitable<void> {
                    co_await fn(std::forward<decltype(args)>(args)..., std::move(req), std::move(res));
                },
                std::move(*values_result));
        };
        return make_handler(std::move(wrapper));
    }
}

} // namespace simple_http
