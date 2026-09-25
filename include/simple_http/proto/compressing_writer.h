#pragma once

// CompressingResponseWriter: wraps an engine's ResponseWriter and compresses the
// body when the client asked for it and the response is worth compressing.
//
// It is installed at the two places a Response is built (h1_engine.h and
// h2_engine.h), so every handler - including the reverse proxy - gets
// compression without knowing about it. It is opt-in through
// CompressionConfig::enabled; when that is off, maybe_compress_writer returns the
// inner writer untouched and no code path here runs.
//
// Two protocol details drive most of this file:
//
//   * Content-Length. HTTP/1.x's one-shot send() writes its own length, so we
//     must not add a second one (duplicate Content-Length is a request-
//     smuggling vector); its streaming path uses chunked and wants none either.
//     HTTP/2's submit_headers forwards whatever it is given and never
//     recomputes, so on its one-shot path the compressed length must be stated
//     explicitly, and a stale one is RFC 9113 §8.1.1 protocol error. See
//     apply_length().
//
//   * Empty chunks. HTTP/1.x's send_chunk("") emits the terminating
//     "0\r\n\r\n", ending the body early, so an empty string must never reach
//     it. A codec legitimately buffers without producing output, so this is a
//     normal case, not an error. See the guard in send_chunk().
//
// The proxy never needs special handling: http_proxy.h copies the upstream's
// Content-Encoding verbatim, and the "already encoded" rule below then skips
// compression, so an upstream that already compressed is passed through
// byte-for-byte and one that did not gets compressed here.

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>

#include "../core/compression.h"
#include "../core/content_encoding.h"
#include "headers.h"
#include "response_writer.h"

namespace simple_http {

namespace asio = boost::asio;

class CompressingResponseWriter final : public ResponseWriter {
  public:
    CompressingResponseWriter(std::shared_ptr<ResponseWriter> inner, asio::any_io_executor executor,
                              CompressionConfig config, std::string encoding, bool head_request)
        : m_inner(std::move(inner)),
          m_executor(std::move(executor)),
          m_config(std::move(config)),
          m_encoding(std::move(encoding)),
          m_head_request(head_request) {}

    // --- one-shot: the only path that knows the full length ---

    asio::awaitable<error_code> send(int status, Headers headers, std::string body) override {
        co_await hop();
        if (m_done) {
            co_return co_await m_inner->send(status, std::move(headers), std::move(body));
        }
        m_done = true;

        if (eligible(status, headers) && body.size() >= m_config.min_bytes) {
            if (auto encoded = encode_all(headers, body)) {
                co_return co_await m_inner->send(status, std::move(headers), std::move(*encoded));
            }
        }
        co_return co_await m_inner->send(status, std::move(headers), std::move(body));
    }

    // --- streaming: the length is not known yet, so min_bytes cannot apply ---

    asio::awaitable<error_code> send_headers(int status, Headers headers) override {
        co_await hop();
        if (m_done || !m_config.compress_streamed || !eligible(status, headers)) {
            m_state = State::Passthrough;
            co_return co_await m_inner->send_headers(status, std::move(headers));
        }
        m_encoder = make_encoder(m_encoding, m_config);
        if (!m_encoder) {  // codec unavailable: fall back for the whole response
            m_state = State::Passthrough;
            co_return co_await m_inner->send_headers(status, std::move(headers));
        }
        m_state = State::Compressing;
        // Streaming: the length is unknown, so it must be absent (HTTP/1.x will
        // add chunked, HTTP/2 frames by DATA).
        apply_length(headers, 0, /*one_shot=*/false);
        decorate(headers);
        co_return co_await m_inner->send_headers(status, std::move(headers));
    }

    asio::awaitable<error_code> send_chunk(std::string data) override {
        co_await hop();
        if (m_done || m_state != State::Compressing) {
            if (data.empty()) {
                co_return error_code{};  // see the header comment: never forward an empty chunk
            }
            co_return co_await m_inner->send_chunk(std::move(data));
        }
        std::string out = m_encoder->write(data);
        if (out.empty()) {
            co_return error_code{};  // buffered by the codec; nothing to send yet
        }
        co_return co_await m_inner->send_chunk(std::move(out));
    }

    asio::awaitable<error_code> send_last(std::string data) override {
        co_await hop();
        if (m_done || m_state != State::Compressing) {
            m_done = true;
            co_return co_await m_inner->send_last(std::move(data));
        }
        m_done = true;
        std::string out = m_encoder->write(data);
        out += m_encoder->finish();
        // Even an empty tail must be forwarded: HTTP/1.x needs it to write the
        // chunked terminator, HTTP/2 to mark the stream end.
        co_return co_await m_inner->send_last(std::move(out));
    }

    // --- never compressed ---

    asio::awaitable<error_code> send_bodyless(int status, Headers headers) override {
        co_await hop();
        m_done = true;
        m_state = State::Passthrough;
        m_encoder.reset();
        // 204/304/HEAD carry no body, so no Content-Encoding either.
        co_return co_await m_inner->send_bodyless(status, std::move(headers));
    }

    asio::awaitable<error_code> send_continue() override {
        co_await hop();
        // Not a state change: it precedes the real response. The reverse proxy
        // calls this directly on the writer when it relays an Expect header.
        co_return co_await m_inner->send_continue();
    }

    // The inner writer hops for us, so this needs no hop of its own.
    asio::awaitable<bool> connected() const override { co_return co_await m_inner->connected(); }

    asio::awaitable<void> close() override {
        // This one touches our own state, so we hop first. The inner close()
        // hops again, which is a plain call when we are already on the executor.
        co_await hop();
        m_done = true;
        m_state = State::Passthrough;
        m_encoder.reset();
        co_await m_inner->close();
    }

    Version version() const override { return m_inner->version(); }

  private:
    enum class State { Passthrough, Compressing };

    // Re-enter the connection's executor before touching any state here, so the
    // decorator keeps the same "safe to write from any thread" promise the inner
    // writer makes by hopping itself.
    asio::awaitable<void> hop() {
        co_await asio::dispatch(asio::bind_executor(m_executor, asio::use_awaitable));
    }

    bool is_http1() const {
        const Version v = m_inner->version();
        return v == Version::Http1 || v == Version::Http11;
    }

    // Everything that can be decided without looking at the body.
    bool eligible(int status, const Headers& headers) const {
        if (m_head_request) {
            return false;  // headers only: the body never exists
        }
        if (status == 204 || status == 304 || status == 206) {
            return false;  // no body, or a byte range that must stay addressable
        }
        if (headers.contains("content-encoding")) {
            return false;  // already encoded (this is what passes a pre-compressed upstream through)
        }
        if (headers.contains("content-range")) {
            return false;
        }
        if (m_config.respect_no_transform) {
            if (auto cc = headers.get("cache-control"); cc && header_has_token(*cc, "no-transform")) {
                return false;
            }
        }
        auto content_type = headers.get("content-type");
        return content_type.has_value() && is_compressible_type(*content_type, m_config);
    }

    // HTTP/1.x computes the length itself (send()) or frames with chunked
    // (send_headers), so it must never see ours. HTTP/2 does neither, so a
    // one-shot response there needs the compressed length spelled out.
    void apply_length(Headers& headers, std::size_t compressed_size, bool one_shot) const {
        headers.erase("content-length");
        if (one_shot && !is_http1()) {
            headers.add_lower("content-length", std::to_string(compressed_size));
        }
    }

    void decorate(Headers& headers) const {
        headers.erase("content-encoding");
        headers.add_lower("content-encoding", m_encoding);
        if (m_config.vary) {
            merge_vary(headers);
        }
        if (m_config.weaken_etag) {
            weaken_etag(headers);
        }
    }

    // Adds Accept-Encoding to Vary, merging with whatever is already there: an
    // upstream may have sent `Vary: Origin`, and overwriting it would break its
    // caching. `Vary: *` already covers everything.
    static void merge_vary(Headers& headers) {
        auto existing = headers.get("vary");
        if (!existing) {
            headers.add_lower("vary", "Accept-Encoding");
            return;
        }
        if (header_has_token(*existing, "*") || header_has_token(*existing, "accept-encoding")) {
            return;
        }
        std::string merged{*existing};
        merged += ", Accept-Encoding";
        headers.erase("vary");
        headers.add_lower("vary", std::move(merged));
    }

    // A compressed body is a different representation, so the strong validator
    // no longer identifies it. Demoting to weak keeps If-None-Match working
    // under the weaker comparison instead of answering with a wrong ETag.
    static void weaken_etag(Headers& headers) {
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
    // response of incompressible bytes can grow - which leaves the caller to
    // send it unchanged. `headers` must not be touched by the caller in that
    // case, so this only mutates them once the win is certain.
    std::optional<std::string> encode_all(Headers& headers, std::string_view body) {
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

    std::shared_ptr<ResponseWriter> m_inner;
    asio::any_io_executor m_executor;
    CompressionConfig m_config;
    std::string m_encoding;
    bool m_head_request{false};
    State m_state{State::Passthrough};
    std::unique_ptr<ContentEncoder> m_encoder;
    bool m_done{false};
};

// The engine-side entry point. Returns `inner` unchanged when compression is off,
// no codec is compiled in, or the client accepts none of what we produce - in
// which case not a single extra object is allocated.
inline std::shared_ptr<ResponseWriter> maybe_compress_writer(std::shared_ptr<ResponseWriter> inner,
                                                             asio::any_io_executor executor,
                                                             const CompressionConfig& config,
                                                             std::string_view accept_encoding,
                                                             bool head_request) {
    auto encoding = negotiate_encoding(accept_encoding, config);
    if (!encoding) {
        return inner;
    }
    return std::make_shared<CompressingResponseWriter>(std::move(inner), std::move(executor), config,
                                                       std::move(*encoding), head_request);
}

}  // namespace simple_http
