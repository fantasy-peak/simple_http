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

namespace asio = boost::asio;
using namespace simple_http;
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

std::string classify(error_code ec) {
    if (!ec) {
        return {};
    }
    if (ec == make_error_code(client_errc::connect_timeout) || ec == make_error_code(client_errc::request_timeout) ||
        ec == make_error_code(client_errc::response_head_timeout) ||
        ec == make_error_code(client_errc::body_idle_timeout)) {
        return "timeout";
    }
    if (ec == make_error_code(client_errc::body_too_large)) {
        return "body_too_large";
    }
    if (ec == make_error_code(client_errc::too_many_redirects)) {
        return "too_many_redirects";
    }
    if (ec == make_error_code(client_errc::redirect_to_insecure)) {
        return "redirect_to_insecure";
    }
    if (ec == make_error_code(client_errc::expectation_failed)) {
        return "expectation_failed";
    }
    if (ec == make_error_code(client_errc::trailers_not_supported)) {
        return "trailers_not_supported";
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
    if (version == "h2c") {
        return {"http://127.0.0.1:7812", HttpVersionPolicy::Http2, H2cMode::PriorKnowledge, false};
    }
    if (version == "h2c-upgrade") {
        return {"http://127.0.0.1:7812", HttpVersionPolicy::Http2, H2cMode::Upgrade, false};
    }
    if (version == "h3") {
        return {"https://127.0.0.1:7813", HttpVersionPolicy::Http3, H2cMode::Off, true};
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
    if (endpoint.tls || (spec.tls_verify.has_value() && !*spec.tls_verify)) {
        cfg.tls.verify_peer = false;
        cfg.tls.verify_host = false;
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
    auto body = co_await response.text();
    if (!body) {
        obs.error = classify(body.error());
        co_return obs;
    }
    set_body(obs, std::move(*body));
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
        auto opened = co_await MultipartStreamWriter::open(client, url, std::move(multipart_options));
        if (!opened) {
            obs.error = classify(opened.error());
            co_return obs;
        }
        for (const auto &part : c.request.body->parts) {
            if (part.filename.empty()) {
                if (auto ec = co_await opened->field(part.name, part.data)) {
                    obs.error = classify(ec);
                    co_return obs;
                }
            } else {
                if (auto ec = co_await opened->file_data(part.name, part.filename, part.content_type, part.data)) {
                    obs.error = classify(ec);
                    co_return obs;
                }
            }
        }
        if (auto ec = co_await opened->finish()) {
            obs.error = classify(ec);
            co_return obs;
        }
        auto head = co_await opened->stream().read_head();
        if (!head) {
            obs.error = classify(head.error());
            co_return obs;
        }
        obs = observe_headers(head->headers, head->status, head->version);
        obs.case_id = c.id;
        obs.mode = c.mode;
        auto body = co_await opened->stream().read_all();
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
        auto opened = co_await client.open_stream(url, std::move(spec));
        if (!opened) {
            obs.error = classify(opened.error());
            co_return obs;
        }
        (void)co_await opened->finish();
        auto head = co_await opened->read_head();
        if (!head) {
            obs.error = classify(head.error());
            co_return obs;
        }
        obs = observe_headers(head->headers, head->status, head->version);
        obs.case_id = c.id;
        obs.mode = c.mode;
        auto body = co_await opened->read_all();
        if (!body) {
            obs.error = classify(body.error());
            co_return obs;
        }
        set_body(obs, std::move(*body));
        for (const auto &[name, value] : opened->trailers()) {
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
    auto response = co_await builder.send();
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
        auto opened = co_await MultipartStreamWriter::open(client, base + "/multipart", {.boundary = "DIFFBOUNDARY"});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await opened->field("title", "hello");
            (void)co_await opened->file_data("upload", "notes.txt", "text/plain", "file data");
            (void)co_await opened->finish();
            auto head = co_await opened->stream().read_head();
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.streaming_multipart";
                auto body = co_await opened->stream().read_all();
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
        auto opened = co_await client.open_stream(base + "/echo", std::move(upload_spec));
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await stream_file_body(*opened, path.string());
            (void)co_await opened->finish();
            auto head = co_await opened->read_head();
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.file_upload";
                auto body = co_await opened->read_all();
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
        auto opened =
            co_await client.open_stream(base + "/expect-ok", {.method = Method::Post, .auto_expect_continue = true});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await opened->write("hello!");
            (void)co_await opened->finish();
            auto head = co_await opened->read_head();
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.expect_ok";
                auto body = co_await opened->read_all();
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
        auto opened = co_await client.open_stream(base + "/expect-reject",
                                                  {.method = Method::Post, .auto_expect_continue = true});
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            const auto ec = co_await opened->write("body");
            if (ec && ec != make_error_code(client_errc::expectation_failed)) {
                obs.error = classify(ec);
            } else {
                auto head = co_await opened->read_head();
                if (!head) {
                    obs.error = classify(head.error());
                } else {
                    obs = observe_headers(head->headers, head->status, head->version);
                    obs.case_id = "body.expect_reject";
                    auto body = co_await opened->read_all();
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
        auto opened = co_await client.open_stream(base + "/request-trailer", std::move(spec));
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await opened->write("body");
            Headers trailers;
            trailers.add("x-check", "yes");
            (void)co_await opened->finish_trailers(std::move(trailers));
            auto head = co_await opened->read_head();
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.request_trailers";
                auto body = co_await opened->read_all();
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
        auto opened = co_await client.open_stream(base + "/trailers");
        if (!opened) {
            obs.error = classify(opened.error());
        } else {
            (void)co_await opened->finish();
            auto head = co_await opened->read_head();
            if (!head) {
                obs.error = classify(head.error());
            } else {
                obs = observe_headers(head->headers, head->status, head->version);
                obs.case_id = "body.response_trailers";
                auto body = co_await opened->read_all();
                if (!body) {
                    obs.error = classify(body.error());
                } else {
                    set_body(obs, std::move(*body));
                    if (const auto value = opened->trailers().get("x-checksum")) {
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
        auto sse = co_await SseStream::open(client, base + "/sse");
        if (!sse) {
            obs.error = classify(sse.error());
        } else {
            std::string data;
            for (;;) {
                auto event = co_await (*sse).next();
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
            obs.status = 200;
            set_body(obs, std::move(data));
        }
        emit(std::move(obs));
    }

    // 8. max_body_bytes.
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
    auto response = co_await client.get(url).send();
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
        (void)co_await client.get(base + "/cookies/set").send();
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

    // Default: no retry; the 429 reaches the caller.
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        Client client{co_await asio::this_coro::executor, cfg};
        (void)co_await client.get(base + "/retry/reset").send();
        emit(co_await get_observation(client, base + "/retry/2", "policy.retry_default_off"));
    }

    // C++ extension: opt-in status retries (Go net/http has no equivalent).
    {
        ClientConfig cfg;
        cfg.default_version = HttpVersionPolicy::Http11;
        cfg.default_h2c = H2cMode::Off;
        cfg.retry.max_retries = 2;
        cfg.retry.retry_statuses = {429};
        cfg.retry.initial_backoff = 1ms;
        Client client{co_await asio::this_coro::executor, cfg};
        (void)co_await client.get(base + "/retry/reset").send();
        auto obs = co_await get_observation(client, base + "/retry/2", "policy.retry_opt_in");
        obs.mode = "cpp-only";
        if (!obs.error && (obs.status != 200 || obs.body != "ok")) {
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
    auto opened = co_await client.open_websocket("ws://127.0.0.1:7810/ws");
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
        if (auto ec = co_await ws->write_text("hello")) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws->read();
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
        if (auto ec = co_await ws->write_binary(binary)) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws->read();
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
        if (auto ec = co_await ws->write_text(std::string(64 * 1024, 'x'))) {
            obs.error = classify(ec);
        } else {
            auto message = co_await ws->read();
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
