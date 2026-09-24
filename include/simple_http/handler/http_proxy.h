#pragma once

// HTTP reverse proxy — request-level, standard forward-proxy behaviour.
//
// Unlike the byte-level WebSocket tunnel (engine/h1/ws_proxy.h), this operates
// on the parsed Request/Response abstraction, one request at a time:
//
//   1. open a plain TCP connection to the backend host:port (one per request);
//   2. send the request line (with rewritten target) + forwarded headers, then
//      stream the request body through;
//   3. read the backend's status line + headers with a small incremental
//      parser;
//   4. stream the backend's response body back to the client through the
//      version-agnostic Response (so it works over HTTP/1.x and HTTP/2 alike).
//
// Standard reverse-proxy header handling (RFC 7230 §5.7.1 / §6.1):
//   * append the client address to X-Forwarded-For, set X-Forwarded-Proto and
//     X-Forwarded-Host;
//   * rewrite Host to the backend authority;
//   * strip hop-by-hop headers (Connection, Keep-Alive, Proxy-*, TE, Trailer,
//     Transfer-Encoding, Upgrade) plus any field named in the Connection header,
//     on both the request and the response.
//
// Concurrency (model A): the handler runs on the connection executor; the
// backend socket is created on that same executor, so all I/O is single-
// threaded. The backend connection is opened and closed per request (no pool).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "../core/logging.h"
#include "../core/types.h"
#include "../engine/dispatcher.h"  // HttpProxyTarget
#include "../proto/headers.h"
#include "../proto/request.h"
#include "../proto/response.h"

namespace simple_http {

namespace asio = boost::asio;

namespace detail {

// Hop-by-hop headers that must not be forwarded (RFC 7230 §6.1). Field names are
// compared lowercased (Headers stores them lowercased already).
inline bool is_hop_by_hop(std::string_view name) {
    static constexpr std::string_view kHop[] = {
        "connection", "keep-alive",         "proxy-authenticate", "proxy-authorization",
        "te",         "trailer",            "transfer-encoding",  "upgrade",
    };
    for (auto h : kHop) {
        if (name == h) return true;
    }
    return false;
}

inline std::string to_lower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Splits a comma-separated Connection header value into lowercased tokens, which
// name additional per-connection headers that must also be stripped.
inline std::vector<std::string> connection_tokens(const Headers& headers) {
    std::vector<std::string> tokens;
    auto conn = headers.get("connection");
    if (!conn) return tokens;
    std::string_view v{*conn};
    std::size_t pos = 0;
    while (pos < v.size()) {
        std::size_t comma = v.find(',', pos);
        std::string_view tok = v.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        // trim OWS
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1);
        if (!tok.empty()) tokens.push_back(to_lower(tok));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return tokens;
}

inline bool contains_token(const std::vector<std::string>& tokens, std::string_view name) {
    for (const auto& t : tokens) {
        if (t == name) return true;
    }
    return false;
}

// Parsed status line + headers of a backend HTTP/1.x response. Body framing is
// decided from these fields by the caller.
struct BackendResponseHead {
    int status = 0;
    Headers headers;
};

// Incremental parser for a backend response head (status line + header block up
// to CRLFCRLF). Mirrors the request-side H1Parser but for responses.
class BackendResponseParser {
  public:
    enum class State { NeedMore, Done, Error };

    void feed(const std::byte* data, std::size_t n) {
        m_buf.append(reinterpret_cast<const char*>(data), n);
    }

    State parse() {
        std::size_t end = m_buf.find("\r\n\r\n");
        std::size_t sep = 4;
        if (end == std::string::npos) {
            end = m_buf.find("\n\n");
            sep = 2;
            if (end == std::string::npos) return State::NeedMore;
        }
        std::string_view block{m_buf.data(), end};
        if (!parse_block(block)) return State::Error;
        m_consumed = end + sep;
        return State::Done;
    }

    const BackendResponseHead& head() const { return m_head; }

    // Bytes buffered past the head — the start of the response body.
    std::string_view remainder() const { return std::string_view{m_buf}.substr(m_consumed); }

    std::size_t buffered() const { return m_buf.size(); }

  private:
    bool parse_block(std::string_view block) {
        std::size_t pos = 0;
        std::string_view line = next_line(block, pos);
        if (!parse_status_line(line)) return false;
        while (pos < block.size()) {
            std::string_view hline = next_line(block, pos);
            if (hline.empty()) continue;
            if (!parse_header_line(hline)) return false;
        }
        return true;
    }

    static std::string_view next_line(std::string_view block, std::size_t& pos) {
        std::size_t nl = block.find('\n', pos);
        std::size_t line_end = (nl == std::string_view::npos) ? block.size() : nl;
        std::size_t raw_end = line_end;
        if (raw_end > pos && block[raw_end - 1] == '\r') raw_end -= 1;
        std::string_view line = block.substr(pos, raw_end - pos);
        pos = (nl == std::string_view::npos) ? block.size() : nl + 1;
        return line;
    }

    bool parse_status_line(std::string_view line) {
        // HTTP-version SP status-code SP reason-phrase
        std::size_t sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) return false;
        std::size_t sp2 = line.find(' ', sp1 + 1);
        std::string_view code = (sp2 == std::string_view::npos) ? line.substr(sp1 + 1)
                                                                : line.substr(sp1 + 1, sp2 - sp1 - 1);
        int status = 0;
        for (char c : code) {
            if (c < '0' || c > '9') return false;
            status = status * 10 + (c - '0');
        }
        if (status < 100 || status > 599) return false;
        m_head.status = status;
        return true;
    }

    bool parse_header_line(std::string_view line) {
        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        std::string_view key = line.substr(0, colon);
        std::size_t vstart = colon + 1;
        while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
        std::string_view value = line.substr(vstart);
        std::size_t vend = value.size();
        while (vend > 0 && (value[vend - 1] == ' ' || value[vend - 1] == '\t')) --vend;
        value = value.substr(0, vend);
        m_head.headers.add(std::string{key}, std::string{value});
        return true;
    }

    std::string m_buf;
    std::size_t m_consumed = 0;
    BackendResponseHead m_head;
};

}  // namespace detail

// Runs one request-level HTTP reverse-proxy exchange: forwards `req` to the
// backend `target` and streams the response back through `res`. On any transport
// error a 502 Bad Gateway is sent (if the response has not started yet).
inline asio::awaitable<void> run_http_proxy(std::shared_ptr<Request> req, std::shared_ptr<Response> res,
                                            bool client_is_tls, HttpProxyTarget target) {
    auto executor = co_await asio::this_coro::executor;
    auto backend = std::make_shared<asio::ip::tcp::socket>(executor);

    auto fail_502 = [&res]() -> asio::awaitable<void> {
        if (res->connected()) {
            (void)co_await res->status(502).content_type("text/plain").send("Bad Gateway");
        }
        co_return;
    };

    // Resolve + connect the backend on this connection's executor.
    asio::ip::tcp::resolver resolver{executor};
    auto [rec, endpoints] = co_await resolver.async_resolve(target.host, std::to_string(target.port),
                                                            asio::as_tuple(asio::use_awaitable));
    if (rec) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy resolve {}:{} failed: {}", target.host, target.port, rec.message());
        co_await fail_502();
        co_return;
    }
    auto [cec, _] = co_await asio::async_connect(*backend, endpoints, asio::as_tuple(asio::use_awaitable));
    if (cec) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy connect {}:{} failed: {}", target.host, target.port, cec.message());
        co_await fail_502();
        co_return;
    }

    auto write_all = [&backend](const std::string& out) -> asio::awaitable<error_code> {
        // Composed async_write: writes the whole buffer or returns an error.
        auto [ec, n] =
            co_await asio::async_write(*backend, asio::buffer(out.data(), out.size()), asio::as_tuple(asio::use_awaitable));
        (void)n;
        co_return ec;
    };

    // --- build the forwarded request head ---
    std::string authority = target.host + ":" + std::to_string(target.port);
    // The rewrite template operates on the path only (Router matches req->path(),
    // which excludes the query). Preserve the original query string so it is not
    // lost when a rewrite is applied. If the rewrite itself already contains a
    // '?', respect it and do not append the original query.
    std::string forwarded_target;
    if (target.rewrite_path.empty()) {
        forwarded_target = std::string{req->target()};
    } else {
        forwarded_target = target.rewrite_path;
        if (forwarded_target.find('?') == std::string::npos && !req->query().empty()) {
            forwarded_target.push_back('?');
            forwarded_target.append(req->query());
        }
    }

    // Defence in depth: never splice a control byte into the request line or a
    // header we synthesize. The engines reject such bytes at parse time (h1 cannot
    // produce them, h2 validates them), but a proxy must not rely on its caller.
    if (contains_ctl(req->method_token()) || contains_ctl(forwarded_target)) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing request line with CR/LF/NUL");
        co_await fail_502();
        co_return;
    }

    std::string head;
    head.append(req->method_token());
    head.push_back(' ');
    head.append(forwarded_target);
    head.append(" HTTP/1.1\r\n");

    // Does the request carry a body? We must decide this *before* writing the
    // request head (to advertise Transfer-Encoding: chunked). Header hints work
    // for HTTP/1.x (Content-Length / Transfer-Encoding) but NOT for HTTP/2,
    // which frames the body with END_STREAM and carries neither header. So we
    // probe the body: read the first frame now; if it is a non-empty data chunk
    // (or a data chunk before EOF) the request has a body. The peeked frame is
    // forwarded first once the head is written. This is protocol-agnostic and
    // correct for h1, h2 and h2c alike.
    // A client that sent `Expect: 100-continue` waits for a go-ahead before sending
    // its body. Answer it ourselves (RFC 9110 §10.1.1 allows an intermediary to) -
    // otherwise this read would block until the client gives up waiting and sends the
    // body anyway, and a client that never gives up would deadlock the request.
    if (auto exp = req->header("expect"); exp && detail::to_lower(*exp).find("100-continue") != std::string::npos) {
        (void)co_await res->writer().send_continue();
    }

    std::string first_chunk;
    bool req_has_body = false;
    {
        auto frame = co_await req->body().read();
        if (frame && !frame->eof) {
            req_has_body = true;
            first_chunk = std::move(frame->data);
        }
        // else: no body (immediate EOF), or a read error — forward with no body.
    }

    // Copy request headers, dropping hop-by-hop + Connection-listed ones, and
    // overriding Host with the backend authority.
    auto req_conn_tokens = detail::connection_tokens(req->headers());
    bool saw_xff = false;
    std::string xff;
    for (const auto& [name, value] : req->headers()) {
        if (detail::is_hop_by_hop(name) || detail::contains_token(req_conn_tokens, name)) continue;
        if (name == "host") continue;              // replaced below
        if (name == "expect") continue;            // answered locally above
        if (name == "content-length") continue;    // re-framed as chunked below
        if (name == "x-forwarded-for") {
            saw_xff = true;
            xff = value;  // will append the client address below
            continue;
        }
        if (contains_ctl(name) || contains_ctl(value)) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy: refusing header with CR/LF/NUL: {}", name);
            co_await fail_502();
            co_return;
        }
        head.append(name);
        head.append(": ");
        head.append(value);
        head.append("\r\n");
    }

    // Host: backend authority.
    head.append("Host: ");
    head.append(authority);
    head.append("\r\n");

    // X-Forwarded-For: append this client's address to any existing chain.
    std::string client_addr = req->peer_address();
    head.append("X-Forwarded-For: ");
    if (saw_xff && !xff.empty()) {
        head.append(xff);
        head.append(", ");
    }
    head.append(client_addr);
    head.append("\r\n");

    // X-Forwarded-Proto / X-Forwarded-Host.
    head.append("X-Forwarded-Proto: ");
    head.append(client_is_tls ? "https" : "http");
    head.append("\r\n");
    if (auto host = req->header("host")) {
        head.append("X-Forwarded-Host: ");
        head.append(*host);
        head.append("\r\n");
    }

    // Re-frame any request body as chunked (so we need no Content-Length), and
    // ask the backend to close after the response so an unlengthed response body
    // is unambiguously delimited by EOF.
    if (req_has_body) {
        head.append("Transfer-Encoding: chunked\r\n");
    }
    head.append("Connection: close\r\n");
    head.append("\r\n");

    if (auto ec = co_await write_all(head); ec) {
        SIMPLE_HTTP_ERROR_LOG("http-proxy write head failed: {}", ec.message());
        co_await fail_502();
        co_return;
    }

    // --- stream the request body to the backend as HTTP/1.1 chunked ---
    if (req_has_body) {
        // Encodes one non-empty buffer as an HTTP/1.1 chunk.
        auto encode_chunk = [](const std::string& data) {
            std::string chunk;
            char size_buf[2 * sizeof(std::size_t) + 1];
            int m = std::snprintf(size_buf, sizeof(size_buf), "%zx", data.size());
            chunk.append(size_buf, static_cast<std::size_t>(m));
            chunk.append("\r\n");
            chunk.append(data);
            chunk.append("\r\n");
            return chunk;
        };

        // Forward the peeked first chunk, then the rest of the stream.
        if (!first_chunk.empty()) {
            if (auto ec = co_await write_all(encode_chunk(first_chunk)); ec) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy write body failed: {}", ec.message());
                co_await fail_502();
                co_return;
            }
        }
        for (;;) {
            auto frame = co_await req->body().read();
            if (!frame || frame->eof) break;
            if (frame->data.empty()) continue;
            if (auto ec = co_await write_all(encode_chunk(frame->data)); ec) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy write body failed: {}", ec.message());
                co_await fail_502();
                co_return;
            }
        }
        if (auto ec = co_await write_all("0\r\n\r\n"); ec) {  // last-chunk terminator
            SIMPLE_HTTP_ERROR_LOG("http-proxy write body terminator failed: {}", ec.message());
            co_await fail_502();
            co_return;
        }
    }

    // --- read the backend response head ---
    detail::BackendResponseParser parser;
    std::string leftover;  // response bytes read past the head (start of body)
    {
        std::array<std::byte, 8192> tmp{};
        auto state = detail::BackendResponseParser::State::NeedMore;
        // Loop so informational responses (100 Continue, 103 Early Hints) are
        // swallowed instead of being mistaken for the final response.
        for (;;) {
        while (state == detail::BackendResponseParser::State::NeedMore) {
            auto [ec, n] = co_await backend->async_read_some(asio::buffer(tmp.data(), tmp.size()),
                                                             asio::as_tuple(asio::use_awaitable));
            if (ec || n == 0) {
                SIMPLE_HTTP_ERROR_LOG("http-proxy read response head failed: {}",
                                      ec ? ec.message() : std::string{"eof"});
                co_await fail_502();
                co_return;
            }
            parser.feed(tmp.data(), n);
            state = parser.parse();
        }
        if (state == detail::BackendResponseParser::State::Error) {
            SIMPLE_HTTP_ERROR_LOG("http-proxy malformed response head from {}:{}", target.host, target.port);
            co_await fail_502();
            co_return;
        }
        if (parser.head().status >= 200) break;  // final response
        // Informational: discard it and parse the next head from what we have.
        leftover.assign(parser.remainder());
        parser = detail::BackendResponseParser{};
        parser.feed(reinterpret_cast<const std::byte*>(leftover.data()), leftover.size());
        leftover.clear();
        state = parser.parse();
        }
        leftover.assign(parser.remainder());
    }

    const auto& bhead = parser.head();

    // Determine backend response body framing.
    bool resp_chunked = false;
    bool resp_has_length = false;
    std::uint64_t resp_length = 0;
    if (auto te = bhead.headers.get("transfer-encoding"); te && detail::to_lower(*te).find("chunked") != std::string::npos) {
        resp_chunked = true;
    } else if (auto cl = bhead.headers.get("content-length")) {
        std::uint64_t len = 0;
        bool ok = !cl->empty();
        for (char c : *cl) {
            if (c < '0' || c > '9') { ok = false; break; }
            len = len * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (ok) {
            resp_has_length = true;
            resp_length = len;
        }
    }
    // A 204/304 or HEAD-style response has no body regardless of headers.
    bool bodyless = (bhead.status == 204 || bhead.status == 304 ||
                     (req->method() == Method::Head));

    // --- start the client response with filtered headers ---
    res->status(bhead.status);
    auto resp_conn_tokens = detail::connection_tokens(bhead.headers);
    for (const auto& [name, value] : bhead.headers) {
        if (detail::is_hop_by_hop(name) || detail::contains_token(resp_conn_tokens, name)) continue;
        // We stream the body, so the upstream Content-Length is replaced by our own
        // framing - except for HEAD, whose response carries the headers a GET would
        // have produced (Content-Length included) and no body at all.
        if (name == "content-length" && req->method() != Method::Head) continue;
        res->header(name, value);
    }
    if (bodyless) {
        // Nothing follows the headers: no framing, no terminating chunk, no body.
        (void)co_await res->send_bodyless();
        backend->close();
        co_return;
    }

    if (auto ec = co_await res->begin(); ec) {
        // Client went away; nothing more to do.
        backend->close();
        co_return;
    }

    // Helper: read more bytes from the backend into `leftover`. Returns false on
    // EOF/error.
    auto pump_more = [&backend, &leftover]() -> asio::awaitable<bool> {
        std::array<std::byte, 8192> tmp{};
        auto [ec, n] = co_await backend->async_read_some(asio::buffer(tmp.data(), tmp.size()),
                                                         asio::as_tuple(asio::use_awaitable));
        if (ec || n == 0) co_return false;
        leftover.append(reinterpret_cast<const char*>(tmp.data()), n);
        co_return true;
    };

    // An upstream that stops mid-body must not be reported to the client as a
    // complete response: finishing cleanly hands the client a truncated body with a
    // valid terminator and no error. Abort instead - HTTP/1.x closes the connection
    // (so the response has no terminating chunk), HTTP/2 resets the stream - and the
    // client sees the failure.
    auto abort_truncated = [&res, &backend](const char* why) -> asio::awaitable<void> {
        SIMPLE_HTTP_ERROR_LOG("http-proxy: {}; aborting the client response", why);
        res->close();
        backend->close();
        co_return;
    };
    bool body_complete = false;

    // --- stream the response body back to the client ---
    if (resp_chunked) {
        // Decode chunked from the backend and re-emit as opaque chunks to the client.
        for (;;) {
            // read a size line
            std::size_t nl;
            while ((nl = leftover.find('\n')) == std::string::npos) {
                if (!co_await pump_more()) {
                    co_await abort_truncated("upstream ended inside a chunk header");
                    co_return;
                }
            }
            std::string size_line = leftover.substr(0, nl);
            leftover.erase(0, nl + 1);
            if (!size_line.empty() && size_line.back() == '\r') size_line.pop_back();
            if (auto semi = size_line.find(';'); semi != std::string::npos) size_line.resize(semi);
            std::uint64_t chunk_len = 0;
            bool ok = !size_line.empty();
            for (char c : size_line) {
                unsigned d;
                if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') d = 10u + static_cast<unsigned>(c - 'a');
                else if (c >= 'A' && c <= 'F') d = 10u + static_cast<unsigned>(c - 'A');
                else { ok = false; break; }
                chunk_len = chunk_len * 16 + d;
            }
            if (!ok) {
                co_await abort_truncated("malformed chunk size line from upstream");
                co_return;
            }
            if (chunk_len == 0) {
                // consume trailer up to blank line
                for (;;) {
                    std::size_t tnl;
                    while ((tnl = leftover.find('\n')) == std::string::npos) {
                        if (!co_await pump_more()) break;
                    }
                    if ((tnl = leftover.find('\n')) == std::string::npos) break;
                    std::string tline = leftover.substr(0, tnl);
                    leftover.erase(0, tnl + 1);
                    if (!tline.empty() && tline.back() == '\r') tline.pop_back();
                    if (tline.empty()) {
                        body_complete = true;  // trailers terminated: the body is whole
                        break;
                    }
                }
                break;
            }
            // read chunk_len bytes + trailing CRLF
            while (leftover.size() < chunk_len + 2) {
                if (!co_await pump_more()) break;
            }
            if (leftover.size() < chunk_len) break;
            std::string data = leftover.substr(0, static_cast<std::size_t>(chunk_len));
            leftover.erase(0, static_cast<std::size_t>(chunk_len));
            // strip trailing CRLF if present
            if (leftover.size() >= 2 && leftover[0] == '\r' && leftover[1] == '\n') leftover.erase(0, 2);
            if (auto ec = co_await res->write(std::move(data)); ec) { backend->close(); co_return; }
        }
    } else if (resp_has_length) {
        std::uint64_t remaining = resp_length;
        // emit whatever is already buffered
        while (remaining > 0) {
            if (leftover.empty()) {
                if (!co_await pump_more()) break;
            }
            std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, leftover.size()));
            std::string data = leftover.substr(0, take);
            leftover.erase(0, take);
            remaining -= take;
            if (auto ec = co_await res->write(std::move(data)); ec) { backend->close(); co_return; }
        }
        body_complete = (remaining == 0);  // a short read means the upstream was cut off
    } else {
        // No length, no chunked: body runs until the backend closes (we sent
        // Connection: close so the backend does too).
        for (;;) {
            if (!leftover.empty()) {
                std::string data = std::move(leftover);
                leftover.clear();
                if (auto ec = co_await res->write(std::move(data)); ec) { backend->close(); co_return; }
            }
            if (!co_await pump_more()) break;
        }
        body_complete = true;  // no length was announced: the close *is* the terminator
    }

    if (body_complete) {
        (void)co_await res->finish();
    } else {
        co_await abort_truncated("upstream body truncated");
    }
    backend->close();
    co_return;
}

}  // namespace simple_http
