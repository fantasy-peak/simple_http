#pragma once

// CompressingResponseSink: wraps an engine's ResponseSink and compresses the
// body when a mounted `middleware::compress` policy covers the request and the
// response is worth compressing (C++17 token-only port).
//
// The policy is read from the Request's state, where `middleware::compress`
// put it, so compression can be mounted globally, on a group, or on one route.
//
// Two protocol details drive most of this file:
//
//   * Content-Length. HTTP/1.x's one-shot send() writes its own length, so we
//     must not add a second one; its streaming path uses chunked and wants none
//     either. HTTP/2's submit_headers forwards whatever it is given and never
//     recomputes, so on its one-shot path the compressed length must be stated
//     explicitly. See apply_length().
//
//   * Empty chunks. HTTP/1.x's send_chunk("") emits the terminating "0\r\n\r\n",
//     ending the body early, so an empty string must never reach it. A codec
//     legitimately buffers without producing output, so this is a normal case.

#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../core/compression.h"
#include "../core/content_encoding.h"
#include "../core/http_status.h"
#include "headers.h"
#include "request.h"
#include "response_writer.h"

namespace simple_http {

namespace asio = boost::asio;

class CompressingResponseSink final : public ResponseSink,
                                      public std::enable_shared_from_this<CompressingResponseSink> {
  public:
    CompressingResponseSink(std::shared_ptr<ResponseSink> inner, asio::any_io_executor executor,
                            std::shared_ptr<Request> request, bool head_request)
        : m_inner(std::move(inner)), m_executor(std::move(executor)), m_request(std::move(request)),
          m_head_request(head_request) {}

    // --- one-shot: the only path that knows the full length ---

    void send(int status, Headers headers, std::string body, WriteCompletion done) override {
        on_executor(
            [status, headers = std::move(headers), body = std::move(body)](CompressingResponseSink &s,
                                                                           WriteCompletion d) mutable {
                if (s.m_done) {
                    s.m_inner->send(status, std::move(headers), std::move(body), std::move(d));
                    return;
                }
                s.m_done = true;
                if (s.resolve() && s.eligible(status, headers) && body.size() >= s.m_config.min_bytes) {
                    if (auto encoded = s.encode_all(headers, body)) {
                        s.m_inner->send(status, std::move(headers), std::move(*encoded), std::move(d));
                        return;
                    }
                }
                s.m_inner->send(status, std::move(headers), std::move(body), std::move(d));
            },
            std::move(done));
    }

    // --- streaming: the length is not known yet, so min_bytes cannot apply ---

    void send_headers(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [status, headers = std::move(headers)](CompressingResponseSink &s, WriteCompletion d) mutable {
                if (s.m_done || !s.resolve() || !s.m_config.compress_streamed || !s.eligible(status, headers)) {
                    s.m_state = State::Passthrough;
                    s.m_inner->send_headers(status, std::move(headers), std::move(d));
                    return;
                }
                s.m_encoder = make_encoder(s.m_encoding, s.m_config);
                if (!s.m_encoder) { // codec unavailable: fall back for the whole response
                    s.m_state = State::Passthrough;
                    s.m_inner->send_headers(status, std::move(headers), std::move(d));
                    return;
                }
                s.m_state = State::Compressing;
                // Streaming: the length is unknown, so it must be absent.
                s.apply_length(headers, 0, /*one_shot=*/false);
                s.decorate(headers);
                s.m_inner->send_headers(status, std::move(headers), std::move(d));
            },
            std::move(done));
    }

    void send_chunk(std::string data, WriteCompletion done) override {
        on_executor(
            [data = std::move(data)](CompressingResponseSink &s, WriteCompletion d) mutable {
                if (s.m_done || s.m_state != State::Compressing) {
                    if (data.empty()) {
                        std::move(d)(error_code{}); // never forward an empty chunk
                        return;
                    }
                    s.m_inner->send_chunk(std::move(data), std::move(d));
                    return;
                }
                std::string out = s.m_encoder->write(data);
                if (out.empty()) {
                    std::move(d)(error_code{}); // buffered by the codec
                    return;
                }
                s.m_inner->send_chunk(std::move(out), std::move(d));
            },
            std::move(done));
    }

    void send_last(std::string data, WriteCompletion done) override {
        on_executor(
            [data = std::move(data)](CompressingResponseSink &s, WriteCompletion d) mutable {
                if (s.m_done || s.m_state != State::Compressing) {
                    s.m_done = true;
                    s.m_inner->send_last(std::move(data), std::move(d));
                    return;
                }
                s.m_done = true;
                std::string out = s.m_encoder->write(data);
                out += s.m_encoder->finish();
                // Even an empty tail must be forwarded: HTTP/1.x needs it to
                // write the chunked terminator, HTTP/2 to mark the stream end.
                s.m_inner->send_last(std::move(out), std::move(d));
            },
            std::move(done));
    }

    void send_trailers(Headers trailers, WriteCompletion done) override {
        on_executor(
            [trailers = std::move(trailers)](CompressingResponseSink &s, WriteCompletion d) mutable {
                if (s.m_done) {
                    s.m_inner->send_trailers(std::move(trailers), std::move(d));
                    return;
                }
                s.m_done = true;
                // Trailer fields are metadata, never compressed - but a
                // compressing response still owes the codec's own trailer (the
                // gzip footer) as a final body chunk before the HTTP trailer
                // section closes the stream.
                if (s.m_state == State::Compressing) {
                    if (std::string tail = s.m_encoder->finish(); !tail.empty()) {
                        auto inner = s.m_inner;
                        inner->send_chunk(std::move(tail), [inner, trailers = std::move(trailers),
                                                            d = std::move(d)](error_code ec) mutable {
                            if (ec) {
                                std::move(d)(ec);
                                return;
                            }
                            inner->send_trailers(std::move(trailers), std::move(d));
                        });
                        return;
                    }
                }
                s.m_inner->send_trailers(std::move(trailers), std::move(d));
            },
            std::move(done));
    }

    // --- never compressed ---

    void send_bodyless(int status, Headers headers, WriteCompletion done) override {
        on_executor(
            [status, headers = std::move(headers)](CompressingResponseSink &s, WriteCompletion d) mutable {
                s.m_done = true;
                s.m_state = State::Passthrough;
                s.m_encoder.reset();
                // 204/304/HEAD carry no body, so no Content-Encoding either.
                s.m_inner->send_bodyless(status, std::move(headers), std::move(d));
            },
            std::move(done));
    }

    void send_continue(WriteCompletion done) override {
        // Not a state change: it precedes the real response.
        m_inner->send_continue(std::move(done));
    }

    // The inner writer hops for us, so this needs no hop of its own.
    void connected(ConnectedCompletion done) const override { m_inner->connected(std::move(done)); }

    void close(WriteCompletion done) override {
        on_executor(
            [](CompressingResponseSink &s, WriteCompletion d) mutable {
                s.m_done = true;
                s.m_state = State::Passthrough;
                s.m_encoder.reset();
                s.m_inner->close(std::move(d));
            },
            std::move(done));
    }

    Version version() const override { return m_inner->version(); }
    asio::any_io_executor get_executor() const override { return m_executor; }
    bool head_request() const noexcept override { return m_head_request || m_inner->head_request(); }

  private:
    enum class State { Passthrough, Compressing };

    // Re-enter the connection's executor before touching any state here, so the
    // decorator keeps the same "safe to write from any thread" promise the
    // inner writer makes by hopping itself. The shared_from_this() keeps the
    // decorator alive if the hop is asynchronous.
    template <typename Fn> void on_executor(Fn &&fn, WriteCompletion done) {
        auto self = shared_from_this();
        asio::dispatch(asio::bind_executor(
            asio::any_io_executor{m_executor},
            [self, fn = std::forward<Fn>(fn), done = std::move(done)]() mutable { fn(*self, std::move(done)); }));
    }

    // Resolves the per-request policy (set by middleware::compress) and the
    // negotiated encoding, once. False means this response is not compressed.
    bool resolve() {
        if (m_resolved) {
            return m_active;
        }
        m_resolved = true;
        // Weak: a strong back-reference closes a cycle whenever the request
        // stores a response-derived object (middleware::session keeps the
        // ResponseWriter to set cookies), which would leak the request,
        // response, sink and (on h1) the engine/transport per request. The
        // request outlives every send()/write() made from the handler, so the
        // lock succeeds for every resolve() that can still choose a policy.
        const auto request = m_request.lock();
        const CompressionConfig *cfg = request ? request->get_state<CompressionConfig>() : nullptr;
        if (!cfg || !cfg->enabled || m_head_request) {
            return m_active = false;
        }
        const auto accept = request->header("accept-encoding");
        auto encoding = negotiate_encoding(accept.value_or(std::string_view{}), *cfg);
        if (!encoding) {
            return m_active = false;
        }
        m_config = *cfg;
        m_encoding = std::move(*encoding);
        return m_active = true;
    }

    bool is_http1() const {
        const Version v = m_inner->version();
        return v == Version::Http1 || v == Version::Http11;
    }

    // Everything that can be decided without looking at the body.
    bool eligible(int status, const Headers &headers) const {
        if (status == status::no_content || status == status::not_modified || status == status::partial_content) {
            return false; // no body, or a byte range that must stay addressable
        }
        if (headers.contains("content-encoding")) {
            return false; // already encoded (a pre-compressed upstream passes through)
        }
        if (headers.contains("content-range")) {
            return false;
        }
        if (m_config.respect_no_transform) {
            if (auto cc = headers.get("cache-control"); cc && value_has_token(*cc, "no-transform")) {
                return false;
            }
        }
        auto content_type = headers.get("content-type");
        return content_type.has_value() && is_compressible_type(*content_type, m_config);
    }

    // HTTP/1.x computes the length itself (send()) or frames with chunked
    // (send_headers), so it must never see ours. HTTP/2 does neither, so a
    // one-shot response there needs the compressed length spelled out.
    void apply_length(Headers &headers, std::size_t compressed_size, bool one_shot) const {
        headers.erase("content-length");
        if (one_shot && !is_http1()) {
            headers.add_lower("content-length", std::to_string(compressed_size));
        }
    }

    void decorate(Headers &headers) const {
        headers.erase("content-encoding");
        headers.add_lower("content-encoding", m_encoding);
        if (m_config.vary) {
            merge_vary(headers);
        }
        if (m_config.weaken_etag) {
            weaken_etag(headers);
        }
    }

    static void merge_vary(Headers &headers) {
        const auto existing = headers.get_all("vary");
        if (existing.empty()) {
            headers.add_lower("vary", "Accept-Encoding");
            return;
        }
        // Vary is a list-valued field and may legally be split over several
        // lines. Reading only the first and then erasing every occurrence
        // silently dropped the other dimensions.
        bool has_star = false;
        bool has_accept_encoding = false;
        std::string merged;
        for (auto value : existing) {
            if (value_has_token(value, "*"))
                has_star = true;
            if (value_has_token(value, "accept-encoding"))
                has_accept_encoding = true;
            if (!merged.empty())
                merged += ", ";
            merged += value;
        }
        if (has_star || has_accept_encoding) {
            return;
        }
        merged += ", Accept-Encoding";
        headers.erase("vary");
        headers.add_lower("vary", std::move(merged));
    }

    // A compressed body is a different representation, so the strong validator
    // no longer identifies it. Demoting to weak keeps If-None-Match working
    // under the weaker comparison instead of answering with a wrong ETag.
    static void weaken_etag(Headers &headers) {
        auto etag = headers.get("etag");
        if (!etag || etag->empty() || *etag == "*" || starts_with_ci(*etag, "W/")) {
            return;
        }
        std::string weak = "W/";
        weak += *etag;
        headers.erase("etag");
        headers.add_lower("etag", std::move(weak));
    }

    // Compresses `body`, adjusting `headers` in place. Returns nullopt when the
    // body did not actually get smaller - compression has overhead, and a
    // response of incompressible bytes can grow.
    std::optional<std::string> encode_all(Headers &headers, std::string_view body) {
        auto encoder = make_encoder(m_encoding, m_config);
        if (!encoder) {
            return std::nullopt;
        }
        std::string packed = encoder->write(body);
        packed += encoder->finish();
        if (packed.size() >= body.size()) {
            return std::nullopt;
        }
        apply_length(headers, packed.size(), /*one_shot=*/true);
        decorate(headers);
        return packed;
    }

    std::shared_ptr<ResponseSink> m_inner;
    asio::any_io_executor m_executor;
    // Weak: see resolve(). The request must not be kept alive by the response
    // chain; a strong reference turns "session stores the writer on the
    // request" into a per-request reference cycle.
    std::weak_ptr<Request> m_request; // source of the per-request policy + Accept-Encoding
    CompressionConfig m_config{};     // resolved lazily from m_request
    std::string m_encoding;
    bool m_head_request{false};
    bool m_resolved{false};
    bool m_active{false};
    State m_state{State::Passthrough};
    std::unique_ptr<ContentEncoder> m_encoder;
    bool m_done{false};
};

// The engine-side entry point. Returns `inner` unchanged when the client
// advertises no codec this build can produce.
inline std::shared_ptr<ResponseSink> maybe_compress_writer(std::shared_ptr<ResponseSink> inner,
                                                           asio::any_io_executor executor,
                                                           std::shared_ptr<Request> request, bool head_request) {
    if (!request || head_request) {
        return inner;
    }
    const auto accept = request->header("accept-encoding");
    if (!accept) {
        return inner;
    }
    // Probe with a policy-enabling config so only the client's Accept-Encoding
    // matters here; the real policy (min_bytes, types, ...) is applied later.
    CompressionConfig probe;
    probe.enabled = true;
    if (!negotiate_encoding(*accept, probe)) {
        return inner;
    }
    return std::make_shared<CompressingResponseSink>(std::move(inner), std::move(executor), std::move(request),
                                                     head_request);
}

} // namespace simple_http
