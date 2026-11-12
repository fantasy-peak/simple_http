// C++ client driver for the client differential.
//
// Reads the same JSON case files as the Go driver and prints one JSON
// observation per case. Both drivers talk to the same Go reference server
// (test/diff/client/go/refserver); the Python runner compares the two outputs.
//
// Groups are separate binaries built from this one file:
//   DIFF_GROUP_BASIC   methods / query / headers / auth / HEAD / versions
//   DIFF_GROUP_BODY    JSON, multipart, streaming upload, 100-continue,
//                      request/response trailers, SSE, max_body_bytes
//   DIFF_GROUP_POLICY  redirects, cookies, compression, retries, timeouts, JWKS
//   DIFF_GROUP_WS      WebSocket text/binary/large
//   DIFF_GROUP_H3      the basic cases over QUIC
//
// The JSONL observation format is in test/diff/support/harness.py.

#include <algorithm>
#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "simple_http.h"

#include "../../../support/coro_compat.h"

namespace asio = boost::asio;
using namespace simple_http;
using namespace simple_http::test;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// case model (mirrors test/diff/client/go/driver/main.go)
// ---------------------------------------------------------------------------

struct HeaderKV {
    std::string name;
    std::string value;
};

struct BodyPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;
};

struct BodySpec {
    std::string kind;
    std::string value;
    std::vector<BodyPart> parts;
};

struct RetrySpec {
    int max_retries{};
    std::vector<int> retry_statuses;
};

struct ClientSpec {
    std::string version{"h1"};
    int max_redirects{};
    std::optional<bool> auto_decompress;
    std::vector<std::string> accept_encodings;
    int request_timeout_ms{};
    std::size_t max_body_bytes{};
    bool cookie_jar{};
    std::optional<RetrySpec> retry;
    std::optional<bool> tls_verify;
    std::string ca_file;
    std::string cert_chain_file;
    std::string private_key_file;
};

struct RequestSpec {
    std::string method{"GET"};
    std::string path{"/"};
    std::vector<HeaderKV> headers;
    std::vector<HeaderKV> query;
    std::optional<BodySpec> body;
};

struct ExpectSpec {
    int status{};
    std::vector<int> status_in;
    std::string body_mode{"exact"};
    std::vector<std::string> compare_headers;
    std::vector<std::string> ignore_headers;
    bool compare_version{};
    bool compare_framing{};
    bool capture_trailers{};
};

struct TestCase {
    std::string id;
    std::string mode{"compare"};
    ClientSpec client;
    RequestSpec request;
    ExpectSpec expect;
};

// ---------------------------------------------------------------------------
// observation and JSON output
// ---------------------------------------------------------------------------

struct Observation {
    std::string case_id;
    std::string target{"cpp"};
    std::string mode;
    std::optional<int> status;
    std::string version;
    std::map<std::string, std::vector<std::string>> headers;
    std::string body;
    std::string body_encoding;
    std::string framing;
    std::optional<std::string> error;
    std::map<std::string, std::string> notes;
};

void json_escape(std::string &out, std::string_view text) {
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr std::string_view hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

std::string quote(std::string_view text) {
    std::string out;
    json_escape(out, text);
    return out;
}

std::string headers_json(const std::map<std::string, std::vector<std::string>> &headers) {
    std::string out = "{";
    bool first = true;
    for (const auto &[name, values] : headers) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += quote(name);
        out.push_back(':');
        out.push_back('[');
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out += quote(values[i]);
        }
        out.push_back(']');
    }
    out.push_back('}');
    return out;
}

std::string to_json(const Observation &o) {
    std::string out = "{";
    out += "\"case\":" + quote(o.case_id);
    out += ",\"target\":" + quote(o.target);
    if (!o.mode.empty()) {
        out += ",\"mode\":" + quote(o.mode);
    }
    if (o.status) {
        out += ",\"status\":" + std::to_string(*o.status);
    }
    out += ",\"version\":" + quote(o.version);
    out += ",\"headers\":" + headers_json(o.headers);
    out += ",\"body\":" + quote(o.body);
    out += ",\"body_encoding\":" + quote(o.body_encoding);
    if (!o.framing.empty()) {
        out += ",\"framing\":" + quote(o.framing);
    }
    if (o.error) {
        out += ",\"error\":" + quote(*o.error);
    }
    if (!o.notes.empty()) {
        out += ",\"notes\":{";
        bool first = true;
        for (const auto &[name, value] : o.notes) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out += quote(name) + ":" + quote(value);
        }
        out.push_back('}');
    }
    out.push_back('}');
    return out;
}

void emit(Observation obs) {
    std::cout << to_json(obs) << '\n';
    std::cout.flush();
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

void set_body(Observation &obs, std::string body) {
    if (valid_utf8(body)) {
        obs.body = std::move(body);
        obs.body_encoding = "utf8";
    } else {
        obs.body = base64_encode(body);
        obs.body_encoding = "base64";
    }
}

// Every library error gets its own slug so an error-path differential cannot
// pass as `other == other`. The four timeout kinds stay collapsed: Go net/http
// reports them all as a context deadline, so a finer split would create noise,
// not information.
std::string classify(error_code ec) {
    if (!ec) {
        return {};
    }
    if (ec.category() == client_category()) {
        switch (static_cast<client_errc>(ec.value())) {
        case client_errc::bad_url:
            return "bad_url";
        case client_errc::unsupported_scheme:
            return "unsupported_scheme";
        case client_errc::protocol_error:
            return "protocol_error";
        case client_errc::version_not_negotiated:
            return "version_not_negotiated";
        case client_errc::header_too_large:
            return "header_too_large";
        case client_errc::body_too_large:
            return "body_too_large";
        case client_errc::body_decode_failed:
            return "body_decode_failed";
        case client_errc::session_busy:
            return "session_busy";
        case client_errc::session_closed:
            return "session_closed";
        case client_errc::body_not_streaming:
            return "body_not_streaming";
        case client_errc::half_duplex:
            return "half_duplex";
        case client_errc::too_many_streams:
            return "too_many_streams";
        case client_errc::stream_reset:
            return "stream_reset";
        case client_errc::stream_refused:
            return "stream_refused";
        case client_errc::goaway:
            return "goaway";
        case client_errc::connect_timeout:
        case client_errc::request_timeout:
        case client_errc::response_head_timeout:
        case client_errc::body_idle_timeout:
            return "timeout";
        case client_errc::invalid_spec:
            return "invalid_spec";
        case client_errc::redirect_to_insecure:
            return "redirect_to_insecure";
        case client_errc::too_many_redirects:
            return "too_many_redirects";
        case client_errc::http_status:
            return "http_status";
        case client_errc::expectation_failed:
            return "expectation_failed";
        case client_errc::trailers_not_supported:
            return "trailers_not_supported";
        case client_errc::proxy_error:
            return "proxy_error";
        case client_errc::proxy_auth_failed:
            return "proxy_auth_failed";
        }
    }
    if (ec == asio::error::connection_refused) {
        return "connect";
    }
    return "other";
}

std::string framing(const std::map<std::string, std::vector<std::string>> &headers) {
    if (auto it = headers.find("transfer-encoding"); it != headers.end()) {
        for (const auto &value : it->second) {
            if (value.find("chunked") != std::string::npos) {
                return "chunked";
            }
        }
    }
    if (headers.contains("content-length")) {
        return "content-length";
    }
    return "none";
}

Method method_for(std::string_view name) {
    const auto method = method_from_string(name);
    return method == Method::Unknown ? Method::Get : method;
}

struct Endpoint {
    std::string base;
    HttpVersionPolicy policy{HttpVersionPolicy::Http11};
    H2cMode h2c{H2cMode::Off};
    bool tls{};
};

Endpoint endpoint_for(std::string_view version) {
    if (version == "h2") {
        return {"https://127.0.0.1:7811", HttpVersionPolicy::Http2, H2cMode::Off, true};
    }
    if (version == "tls") {
        return {"https://127.0.0.1:7811", HttpVersionPolicy::Http11, H2cMode::Off, true};
    }
    if (version == "h2c") {
        return {"http://127.0.0.1:7812", HttpVersionPolicy::Http2, H2cMode::PriorKnowledge, false};
    }
    if (version == "h2c-upgrade") {
        return {"http://127.0.0.1:7812", HttpVersionPolicy::Http2, H2cMode::Upgrade, false};
    }
    if (version == "h3") {
        return {"https://127.0.0.1:7813", HttpVersionPolicy::Http3, H2cMode::Off, true};
    }
    if (version == "mtls") {
        return {"https://127.0.0.1:7814", HttpVersionPolicy::Http11, H2cMode::Off, true};
    }
    return {"http://127.0.0.1:7810", HttpVersionPolicy::Http11, H2cMode::Off, false};
}

std::string append_query(std::string path, const std::vector<HeaderKV> &query) {
    if (query.empty()) {
        return path;
    }
    path += path.find('?') == std::string::npos ? '?' : '&';
    bool first = true;
    for (const auto &pair : query) {
        if (!first) {
            path.push_back('&');
        }
        first = false;
        path += query_escape(pair.name);
        path.push_back('=');
        path += query_escape(pair.value);
    }
    return path;
}

ClientConfig make_config(const ClientSpec &spec) {
    const auto endpoint = endpoint_for(spec.version);
    ClientConfig cfg;
    cfg.default_version = endpoint.policy;
    cfg.default_h2c = endpoint.h2c;
    cfg.max_redirects = static_cast<std::size_t>(std::max(0, spec.max_redirects));
    cfg.auto_decompress = spec.auto_decompress.value_or(true);
    if (!spec.accept_encodings.empty()) {
        cfg.accept_encodings = spec.accept_encodings;
    }
    if (spec.request_timeout_ms > 0) {
        cfg.request_timeout = std::chrono::milliseconds{spec.request_timeout_ms};
    }
    if (spec.cookie_jar) {
        cfg.cookie_jar = std::make_shared<CookieJar>();
    }
    if (spec.retry) {
        cfg.retry.max_retries = static_cast<unsigned>(std::max(0, spec.retry->max_retries));
        cfg.retry.retry_statuses = spec.retry->retry_statuses;
        cfg.retry.initial_backoff = 1ms;
    }
    if (endpoint.tls) {
        const bool verify = spec.tls_verify.value_or(false);
        cfg.tls.verify_peer = verify;
        cfg.tls.verify_host = verify;
        if (!spec.ca_file.empty()) {
            cfg.tls.ca_file = spec.ca_file;
        }
        cfg.tls.cert_chain_file = spec.cert_chain_file;
        cfg.tls.private_key_file = spec.private_key_file;
    }
    return cfg;
}

Observation base_observation(const TestCase &c) {
    Observation obs;
    obs.case_id = c.id;
    obs.mode = c.mode;
    return obs;
}

Observation observe_headers(const Headers &headers, int status, Version version) {
    Observation obs;
    obs.status = status;
    obs.version = std::string{to_string(version)};
    for (const auto &[name, value] : headers) {
        obs.headers[name].push_back(value);
    }
    obs.framing = framing(obs.headers);
    return obs;
}

asio::awaitable<Observation> observe_response(Response &response) {
    Observation obs = observe_headers(response.headers(), response.status(), response.version());
    set_body(obs, response.text());
    co_return obs;
}

// ---------------------------------------------------------------------------
// declarative engine
// ---------------------------------------------------------------------------

asio::awaitable<Observation> run_case(const TestCase &c) {
    Observation obs = base_observation(c);
    const auto endpoint = endpoint_for(c.client.version);

    if (c.request.body && c.request.body->kind == "multipart") {
        Client client{co_await asio::this_coro::executor, make_config(c.client)};
        const std::string url = endpoint.base + append_query(c.request.path, c.request.query);
        MultipartStreamOptions multipart_options;
        multipart_options.boundary = "DIFFBOUNDARY";
        multipart_options.spec.headers.add("user-agent", "diff-probe");
        for (const auto &header : c.request.headers) {
            multipart_options.spec.headers.add(header.name, header.value);
        }
        auto opened = co_await client_open_multipart(client, url, std::move(multipart_options));
        if (!opened) {
            obs.error = classify(opened.error());
            co_return obs;
        }
        for (const auto &part : c.request.body->parts) {
            if (part.filename.empty()) {
                if (auto ec = co_await mp_field(**opened, part.name, part.data)) {
                    obs.error = classify(ec);
                    co_return obs;
                }
            } else {
                if (auto ec = co_await mp_file_data(**opened, part.name, part.filename, part.content_type, part.data)) {
                    obs.error = classify(ec);
                    co_return obs;
                }
            }
        }
        if (auto ec = co_await finish_any(**opened)) {
            obs.error = classify(ec);
            co_return obs;
        }
        auto head = co_await stream_read_head(*(*opened)->stream());
        if (!head) {
            obs.error = classify(head.error());
            co_return obs;
        }
        obs = observe_headers(head->headers, head->status, head->version);
        obs.case_id = c.id;
        obs.mode = c.mode;
        auto body = co_await stream_read_all(*(*opened)->stream());
        if (!body) {
            obs.error = classify(body.error());
            co_return obs;
        }
        set_body(obs, std::move(*body));
        co_return obs;
    }

    Client client{co_await asio::this_coro::executor, make_config(c.client)};
    const std::string url = endpoint.base + append_query(c.request.path, c.request.query);

    if (c.expect.capture_trailers) {
        StreamSpec spec;
        spec.method = method_for(c.request.method);
        bool saw_agent = false;
        for (const auto &header : c.request.headers) {
            spec.headers.add(header.name, header.value);
            saw_agent = saw_agent || iequals_ci(header.name, "user-agent");
        }
        if (!saw_agent) {
            spec.headers.add("user-agent", "diff-probe");
        }
        auto opened = co_await client_open_stream(client, url, std::move(spec));
        if (!opened) {
            obs.error = classify(opened.error());
            co_return obs;
        }
        (void)co_await finish_any(**opened);
        auto head = co_await stream_read_head(**opened);
        if (!head) {
            obs.error = classify(head.error());
            co_return obs;
        }
        obs = observe_headers(head->headers, head->status, head->version);
        obs.case_id = c.id;
        obs.mode = c.mode;
        auto body = co_await stream_read_all(**opened);
        if (!body) {
            obs.error = classify(body.error());
            co_return obs;
        }
        set_body(obs, std::move(*body));
        for (const auto &[name, value] : (*opened)->trailers()) {
            obs.notes["trailer." + name] = value;
        }
        co_return obs;
    }

    auto builder = client.request(method_for(c.request.method), url);
    for (const auto &header : c.request.headers) {
        builder.header(header.name, header.value);
    }
    bool has_content_type = false;
    bool has_user_agent = false;
    for (const auto &header : c.request.headers) {
        if (iequals_ci(header.name, "content-type")) {
            has_content_type = true;
        }
        if (iequals_ci(header.name, "user-agent")) {
            has_user_agent = true;
        }
    }
    if (!has_user_agent) {
        builder.header("user-agent", "diff-probe");
    }
    if (c.request.body && c.request.body->kind != "none") {
        builder.body(c.request.body->value);
        if (c.request.body->kind == "json" && !has_content_type) {
            builder.content_type(mime::app_json);
        }
    }
    if (c.client.max_body_bytes != 0) {
        builder.max_body_bytes(c.client.max_body_bytes);
    }
    auto response = co_await client_send(builder);
    if (!response) {
        obs.error = classify(response.error());
        co_return obs;
    }
    auto observed = co_await observe_response(*response);
    observed.case_id = c.id;
    observed.mode = c.mode;
    co_return observed;
}

asio::awaitable<void> run_declarative(const std::string &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        std::cerr << "cannot open cases file: " << path << '\n';
        co_return;
    }
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = glz::read_json<std::vector<TestCase>>(text);
    if (!parsed) {
        std::cerr << "cannot parse cases file: " << path << '\n';
        co_return;
    }
    for (const auto &c : *parsed) {
        if (c.mode == "go-only") {
            continue;
        }
        auto obs = co_await run_case(c);
        // A cpp-only case has no Go counterpart to diff against, so assert the
        // parts of `expect` the single-side runner can check here.
        if (c.mode == "cpp-only" && !obs.error && c.expect.status != 0 && obs.status != c.expect.status) {
            obs.error = "assert_failed";
        }
        emit(std::move(obs));
    }
}

// ---------------------------------------------------------------------------
// streaming body group
// ---------------------------------------------------------------------------

asio::awaitable<void> run_body_group() {
    const std::string base = "http://127.0.0.1:7810";
    ClientConfig cfg;
    cfg.default_version = HttpVersionPolicy::Http11;
    cfg.default_h2c = H2cMode::Off;
    Client client{co_await asio::this_coro::executor, cfg};

    // 1. Streaming multipart: field + in-memory file part.
    {
        Observation obs;
        obs.case_id = "body.streaming_multipart";
        auto opened = co_await client_open_multipart(client, base + "/multipart", {.boundary = "DIFFBOUNDARY"});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await mp_field(**opened, "title", "hello");
            (void)co_await mp_file_data(**opened, "upload", "notes.txt", "text/plain", "file data");
            (void)co_await finish_any(**opened);
            auto head = co_await stream_read_head(*(*opened)->stream());
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.streaming_multipart";
                auto body = co_await stream_read_all(*(*opened)->stream());
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                }
            }
        }
        emit(std::move(obs));
    }

    // 2. Raw file-body upload.
    {
        Observation obs;
        obs.case_id = "body.file_upload";
        const auto path = std::filesystem::temp_directory_path() / "simple_http_diff_upload.bin";
        std::string payload(200000, 'a');
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>('a' + (i % 26));
        }
        {
            std::ofstream out{path, std::ios::binary};
            out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        }
        StreamSpec upload_spec;
        upload_spec.method = Method::Put;
        upload_spec.headers.add("user-agent", "diff-probe");
        auto opened = co_await client_open_stream(client, base + "/echo", std::move(upload_spec));
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await as_ec(
                async_stream_file_body(*opened, path.string(), 0, asio::as_tuple(asio::use_awaitable)));
            (void)co_await finish_any(**opened);
            auto head = co_await stream_read_head(**opened);
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.file_upload";
                auto body = co_await stream_read_all(**opened);
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                }
            }
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        emit(std::move(obs));
    }

    // 3. Expect: 100-continue accepted.
    {
        Observation obs;
        obs.case_id = "body.expect_ok";
        auto opened = co_await client_open_stream(client, base + "/expect-ok",
                                                  {.method = Method::Post, .auto_expect_continue = true});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await stream_write(**opened, "hello!");
            (void)co_await finish_any(**opened);
            auto head = co_await stream_read_head(**opened);
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.expect_ok";
                auto body = co_await stream_read_all(**opened);
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                }
            }
        }
        emit(std::move(obs));
    }

    // 4. Expect rejected by a final response.
    {
        Observation obs;
        obs.case_id = "body.expect_reject";
        auto opened = co_await client_open_stream(client, base + "/expect-reject",
                                                  {.method = Method::Post, .auto_expect_continue = true});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            const auto ec = co_await stream_write(**opened, "body");
            if (ec && ec != make_error_code(client_errc::expectation_failed)) {
                obs.error = classify(ec);
            } else {
                auto head = co_await stream_read_head(**opened);
                if (!head) {
                    obs.error = classify(head.error());
                } else {
                    obs = observe_headers(head->headers, head->status, head->version);
                    obs.case_id = "body.expect_reject";
                    auto body = co_await stream_read_all(**opened);
                    if (!body) {
                        obs.error = classify(body.error());
                    } else {
                        set_body(obs, std::move(*body));
                    }
                }
            }
        }
        emit(std::move(obs));
    }

    // 5. Request trailers.
    {
        Observation obs;
        obs.case_id = "body.request_trailers";
        StreamSpec spec;
        spec.method = Method::Post;
        spec.headers.add("trailer", "x-check");
        auto opened = co_await client_open_stream(client, base + "/request-trailer", std::move(spec));
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await stream_write(**opened, "body");
            Headers trailers;
            trailers.add("x-check", "yes");
            (void)co_await stream_finish_trailers(**opened, std::move(trailers));
            auto head = co_await stream_read_head(**opened);
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.request_trailers";
                auto body = co_await stream_read_all(**opened);
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                }
            }
        }
        emit(std::move(obs));
    }

    // 6. Response trailers.
    {
        Observation obs;
        obs.case_id = "body.response_trailers";
        auto opened = co_await client_open_stream(client, base + "/trailers");
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await finish_any(**opened);
            auto head = co_await stream_read_head(**opened);
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.response_trailers";
                auto body = co_await stream_read_all(**opened);
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                    if (const auto value = (*opened)->trailers().get("x-checksum")) {
                        obs.notes["trailer.x-checksum"] = std::string{*value};
                    }
                }
            }
        }
        emit(std::move(obs));
    }

    // 7. SSE.
    {
        Observation obs;
        obs.case_id = "body.sse";
        auto sse = co_await client_open_sse(client, base + "/sse");
        if (!sse) {
            obs.error = classify(sse.error());
        } else {
            std::string data;
            for (;;) {
                auto event = co_await sse_next(**sse);
                if (!event) {
                    break;
                }
                if (!data.empty()) {
                    data.push_back('|');
                }
                std::string value = event->data;
                while (!value.empty() && value.back() == '\n') {
                    value.pop_back();
                }
                data += value;
            }
            obs.status = status::ok;
            set_body(obs, std::move(data));
        }
        emit(std::move(obs));
    }

    // 8. SSE reconnect: Last-Event-ID resumes after the previous id.
    {
        Observation obs;
        obs.case_id = "body.sse_resume";
        SseOptions options;
        options.last_event_id = "2";
        auto sse = co_await client_open_sse(client, base + "/sse-resume", options);
        if (!sse) {
            obs.error = classify(sse.error());
        } else {
            std::string data;
            for (;;) {
                auto event = co_await sse_next(**sse);
                if (!event) {
                    break;
                }
                if (!data.empty()) {
                    data.push_back('|');
                }
                std::string value = event->data;
                while (!value.empty() && value.back() == '\n') {
                    value.pop_back();
                }
                data += value;
            }
            obs.status = status::ok;
            set_body(obs, std::move(data));
        }
        emit(std::move(obs));
    }

    // 9. max_body_bytes.
    {
        TestCase c;
        c.id = "body.max_body_bytes";
        c.client.max_body_bytes = 1024;
        c.request.path = "/large?n=100000";
        emit(co_await run_case(c));
    }
}

// ---------------------------------------------------------------------------
// policy group (redirects / compression / JWKS are declarative; the rest here)
// ---------------------------------------------------------------------------

asio::awaitable<Observation> get_observation(Client &client, const std::string &url, const std::string &case_id) {
    auto response = co_await client_send(client.get(url).header("user-agent", "diff-probe"));
    Observation obs;
    obs.case_id = case_id;
    if (!response) {
        obs.error = classify(response.error());
        co_return obs;
    }
    obs = co_await observe_response(*response);
    obs.case_id = case_id;
    co_return obs;
}

asio::awaitable<void> run_policy_group() {
    const std::string base = "http://127.0.0.1:7810";

    // CookieJar across requests.
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        cfg.cookie_jar = std::make_shared<CookieJar>();
        Client client{co_await asio::this_coro::executor, cfg};
        (void)co_await client_send(client.get(base + "/cookies/set"));
        emit(co_await get_observation(client, base + "/cookies", "policy.cookies"));
    }

    // Request timeout.
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        cfg.request_timeout = 200ms;
        Client client{co_await asio::this_coro::executor, cfg};
        emit(co_await get_observation(client, base + "/slow?ms=2000", "policy.timeout"));
    }

    // RequestBuilder::timeout(head, idle) is layered: head bounds the wait for
    // the response head, idle bounds silence between body chunks. Go has no
    // body-idle knob, so these are C++-only assertions on the exact error.
    auto builder_timeout_case = [&](std::string case_id, std::string path, std::chrono::milliseconds head,
                                    std::chrono::milliseconds idle, client_errc want) -> asio::awaitable<void> {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        Client client{co_await asio::this_coro::executor, cfg};
        auto response =
            co_await client_send(client.get(base + path).header("user-agent", "diff-probe").timeout(head, idle));
        Observation obs;
        obs.case_id = case_id;
        obs.mode = "cpp-only";
        if (response) {
            obs.error = "expected_timeout_response_succeeded";
        } else if (response.error() != make_error_code(want)) {
            obs.error = std::string{"wrong_error: "} + response.error().message();
        }
        emit(std::move(obs));
    };
    co_await builder_timeout_case("policy.builder_head_timeout", "/slow?ms=2000", 100ms, {},
                                  client_errc::response_head_timeout);
    co_await builder_timeout_case("policy.builder_body_idle_timeout", "/stall?ms=2000", {}, 100ms,
                                  client_errc::body_idle_timeout);

    // Outbound proxies: a CONNECT proxy, a SOCKS5 proxy, and a proxy that is
    // bypassed by NO_PROXY. The origin is the TLS reference listener.
    auto proxy_case = [&](std::string case_id, ProxyKind kind, std::uint16_t proxy_port,
                          std::vector<std::string> no_proxy) -> asio::awaitable<void> {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        cfg.proxy_from_env = false;
        cfg.proxy = ProxyConfig{.kind = kind, .host = "127.0.0.1", .port = proxy_port};
        cfg.no_proxy = std::move(no_proxy);
        cfg.tls.verify_peer = false;
        cfg.tls.verify_host = false;
        Client client{co_await asio::this_coro::executor, cfg};
        emit(co_await get_observation(client, "https://127.0.0.1:7811/echo", case_id));
    };
    co_await proxy_case("policy.proxy_connect", ProxyKind::Http, 7816, {});
    co_await proxy_case("policy.proxy_socks5", ProxyKind::Socks5, 7817, {});
    co_await proxy_case("policy.no_proxy", ProxyKind::Http, 7816, {"127.0.0.1"});

    // Default: no retry; the 429 reaches the caller.
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        Client client{co_await asio::this_coro::executor, cfg};
        (void)co_await client_send(client.get(base + "/retry/reset"));
        emit(co_await get_observation(client, base + "/retry/2", "policy.retry_default_off"));
    }

    // C++ extension: opt-in status retries (Go net/http has no equivalent).
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        cfg.retry.max_retries = 2;
        cfg.retry.retry_statuses = {status::too_many_requests};
        cfg.retry.initial_backoff = 1ms;
        Client client{co_await asio::this_coro::executor, cfg};
        (void)co_await client_send(client.get(base + "/retry/reset"));
        auto obs = co_await get_observation(client, base + "/retry/2", "policy.retry_opt_in");
        obs.mode = "cpp-only";
        if (!obs.error && (obs.status != status::ok || obs.body != "ok")) {
            obs.error = "assert_failed";
        }
        emit(std::move(obs));
    }
}

// ---------------------------------------------------------------------------
// WebSocket group
// ---------------------------------------------------------------------------

asio::awaitable<void> run_ws_group() {
    ClientConfig cfg;
    cfg.default_version = HttpVersionPolicy::Http11;
    Client client{co_await asio::this_coro::executor, cfg};
    auto opened = co_await client_open_websocket(client, "ws://127.0.0.1:7810/ws");
    if (!opened) {
        for (const char *name : {"ws.text", "ws.binary", "ws.large"}) {
            Observation obs;
            obs.case_id = name;
            obs.error = classify(opened.error());
            emit(std::move(obs));
        }
        co_return;
    }
    auto &ws = *opened;

    // text
    {
        Observation obs;
        obs.case_id = "ws.text";
        if (auto ec = co_await ws_write_text(*ws, "hello")) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws_read(*ws);
            if (!message) {
                obs.error = classify(message.error());
            } else {
                set_body(obs, message->data);
            }
        }
        emit(std::move(obs));
    }
    // binary
    {
        Observation obs;
        obs.case_id = "ws.binary";
        const std::string binary{"\x00\x01\xff\xfe", 4};
        if (auto ec = co_await ws_write_binary(*ws, binary)) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws_read(*ws);
            if (!message) {
                obs.error = classify(message.error());
            } else {
                set_body(obs, message->data);
            }
        }
        emit(std::move(obs));
    }
    // large
    {
        Observation obs;
        obs.case_id = "ws.large";
        if (auto ec = co_await ws_write_text(*ws, std::string(64 * 1024, 'x'))) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws_read(*ws);
            if (!message) {
                obs.error = classify(message.error());
            } else {
                set_body(obs, message->data);
            }
        }
        emit(std::move(obs));
    }
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

#ifdef DIFF_GROUP_BASIC
constexpr std::string_view kGroup = "basic";
#elif defined(DIFF_GROUP_BODY)
constexpr std::string_view kGroup = "body";
#elif defined(DIFF_GROUP_POLICY)
constexpr std::string_view kGroup = "policy";
#elif defined(DIFF_GROUP_WS)
constexpr std::string_view kGroup = "ws";
#elif defined(DIFF_GROUP_H3)
constexpr std::string_view kGroup = "h3";
#else
constexpr std::string_view kGroup = "basic";
#endif

asio::awaitable<void> run_group(std::string cases_path) {
    if (kGroup == "body") {
        co_await run_body_group();
    } else if (kGroup == "ws") {
        co_await run_ws_group();
    } else if (kGroup == "policy") {
        if (!cases_path.empty()) {
            co_await run_declarative(cases_path);
        }
        co_await run_policy_group();
    } else {
        co_await run_declarative(cases_path);
    }
}

} // namespace

int main(int argc, char **argv) {
    set_log_sink(make_stderr_sink(LogLevel::Error));
    std::string cases_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--cases" && i + 1 < argc) {
            cases_path = argv[++i];
        }
    }
    asio::io_context io;
    asio::co_spawn(io, run_group(cases_path), asio::detached);
    io.run();
    return 0;
}
