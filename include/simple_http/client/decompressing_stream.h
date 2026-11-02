#pragma once

// DecompressingClientStream: a ClientStream decorator that decodes the response
// body when it arrives with a Content-Encoding this build can handle.
//
// Decoding happens in read(), not in the convenience layer, so every caller
// sees the original bytes - and, usefully, so do the size caps: read_all()
// (client_stream.h) and the aggregate in HttpClient::read_exchange both bound
// what read() returns, which is now the *decoded* size. A compression bomb
// cannot slip past them by being small on the wire.
//
// The head is rewritten as well. Once the body is decoded it is no longer the
// encoded representation, so Content-Encoding is dropped; Content-Length no
// longer describes what follows, so it goes too. The rewrite is published with
// set_head() so the non-virtual head()/status()/read_head() report it.

#include <algorithm>
#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/content_encoding.h"
#include "../core/types.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http::detail {

namespace asio = boost::asio;

// What the client advertised for this exchange, and therefore what it is
// allowed to decode. Go's Transport only decompresses a response when it was
// the layer that added Accept-Encoding; this carries that decision from
// start_exchange() to the decorator.
struct ResponseDecoding {
    bool enabled{false};
    std::vector<std::string> encodings; // canonical names: "gzip", "br"
};

// Remembered on the Request when start_exchange() injected the header, so a
// caller that reuses the same Request for another explicit stream still gets
// the Go semantics instead of treating the header as caller-supplied.
struct InjectedAcceptEncoding {
    std::string header_value;
    std::vector<std::string> encodings;
};

class DecompressingClientStream final : public ClientStream {
  public:
    DecompressingClientStream(std::shared_ptr<ClientStream> inner, ResponseDecoding policy)
        : m_inner(std::move(inner)), m_policy(std::move(policy)) {}

    // --- request side: nothing to decode on the way out ---
    asio::awaitable<error_code> write(std::string data) override { return m_inner->write(std::move(data)); }
    asio::awaitable<error_code> finish(std::string data) override { return m_inner->finish(std::move(data)); }
    asio::awaitable<error_code> finish_trailers(Headers trailers) override {
        co_return co_await m_inner->finish_trailers(std::move(trailers));
    }

    // --- response side ---
    asio::awaitable<std::expected<ReadResult, error_code>> read() override {
        // The head is what decides whether there is anything to decode, so it has
        // to be read before the first body byte goes out. A caller that goes
        // straight to read()/read_all() - as opposed to the convenience layer,
        // which reads the head first - would otherwise get the encoded bytes
        // passed through untouched. read_head() caches, so this costs a check.
        if (auto head = co_await read_head(); !head) {
            co_return std::unexpected{head.error()};
        }

        if (!m_decoder) {
            co_return co_await m_inner->read();
        }

        // An eof ReadResult carries no data - the aggregate path drops it - so
        // anything the decoder still held back goes out as a chunk of its own,
        // before the end-of-body is reported.
        if (!m_tail.empty()) {
            std::string out = std::move(m_tail);
            m_tail.clear();
            co_return ReadResult{std::move(out), false};
        }
        if (m_inner_eof) {
            co_return ReadResult{std::string{}, true};
        }

        for (;;) {
            auto chunk = co_await m_inner->read();
            if (!chunk) {
                co_return std::unexpected{chunk.error()};
            }
            if (chunk->eof) {
                m_inner_eof = true;
                m_tail = m_decoder->finish();
                if (m_decoder->failed()) {
                    // Half a body must not be handed over as if it were whole.
                    co_return std::unexpected{make_error_code(client_errc::body_decode_failed)};
                }
                if (!m_tail.empty()) {
                    std::string out = std::move(m_tail);
                    m_tail.clear();
                    co_return ReadResult{std::move(out), false};
                }
                co_return ReadResult{std::string{}, true};
            }
            std::string out = m_decoder->write(chunk->data);
            if (m_decoder->failed()) {
                co_return std::unexpected{make_error_code(client_errc::body_decode_failed)};
            }
            if (!out.empty()) {
                co_return ReadResult{std::move(out), false};
            }
            // The codec is buffering; pull again rather than hand the caller an
            // empty chunk it would have to special-case. The inner read() ends
            // the loop by reporting eof or an error.
        }
    }

    void clear_head_cache() override {
        reset_head();
        m_inner->clear_head_cache();
    }

    bool finished() const override { return m_inner->finished(); }
    // The trailer section is parsed by the inner stream after the body; the
    // decoder sits between the caller and it, so it must forward them (and the
    // reverse proxy reads them through this layer).
    [[nodiscard]] const Headers &response_trailers() const noexcept override { return m_inner->response_trailers(); }
    Version version() const override { return m_inner->version(); }
    std::uint32_t id() const override { return m_inner->id(); }
    asio::awaitable<void> cancel() override { co_return co_await m_inner->cancel(); }

  protected:
    asio::awaitable<error_code> await_head() override {
        // The inner await_head() is protected, so go through its public
        // read_head(); the head is cached, so this stays idempotent.
        auto head = co_await m_inner->read_head();
        if (!head) {
            co_return head.error();
        }
        // A HEAD response (and 204/304) may still carry the Content-Encoding a
        // GET would have used, but there is no body to decode. Installing a
        // decoder there makes read() call finish() with no input, which every
        // codec reports as a truncated stream (body_decode_failed).
        if (!head->bodyless) {
            const auto values = head->headers.get_all("content-encoding");
            if (values.size() == 1) {
                const auto coding = parse_content_coding(values.front());
                std::string canonical;
                if (coding == ContentCoding::Gzip) {
                    canonical = "gzip";
                } else if (coding == ContentCoding::Brotli) {
                    canonical = "br";
                }
                const bool advertised =
                    !canonical.empty() && std::find(m_policy.encodings.begin(), m_policy.encodings.end(), canonical) !=
                                              m_policy.encodings.end();
                if (advertised) {
                    m_decoder = make_decoder(canonical);
                    if (m_decoder) {
                        head->headers.erase("content-encoding");
                        head->headers.erase("content-length");
                        head->uncompressed = true;
                    }
                }
                // Anything we did not advertise (including an explicit
                // caller-set Accept-Encoding, a multi-coding list, or an
                // unknown coding) passes through untouched.
            }
        }
        const bool interim = head->status == 100;
        set_head(std::move(*head));
        if (interim) {
            // A 100-continue is handed to the caller once, then the final head
            // follows. The base read_head() returns *m_head immediately, so
            // publishing it with m_head_ready left false gives the caller the
            // interim now and re-enters await_head() on the next call. Caching
            // it would make a read_head() loop spin on 100 forever.
            reset_head();
        }
        co_return error_code{};
    }

  private:
    std::shared_ptr<ClientStream> m_inner;
    ResponseDecoding m_policy;
    std::unique_ptr<ContentDecoder> m_decoder;
    std::string m_tail;      // decoded bytes still owed to the caller
    bool m_inner_eof{false}; // the inner stream has reported end-of-body
};

// Canonicalises the configured encodings, keeping only those this build can
// actually decode and dropping duplicates. Never advertise something we would
// then have to pass through raw.
inline std::vector<std::string> normalize_accept_encodings(const std::vector<std::string> &wanted) {
    std::vector<std::string> out;
    for (const std::string &encoding : wanted) {
        const auto coding = parse_content_coding(encoding);
        std::string canonical;
        if (coding == ContentCoding::Gzip) {
            canonical = "gzip";
        } else if (coding == ContentCoding::Brotli) {
            canonical = "br";
        } else {
            continue;
        }
        if (std::find(out.begin(), out.end(), canonical) == out.end()) {
            out.push_back(std::move(canonical));
        }
    }
    return out;
}

inline std::string join_encodings(const std::vector<std::string> &encodings) {
    std::string out;
    for (const std::string &encoding : encodings) {
        if (!out.empty()) {
            out += ", ";
        }
        out += encoding;
    }
    return out;
}

inline std::string accept_encoding_value(const std::vector<std::string> &wanted) {
    return join_encodings(normalize_accept_encodings(wanted));
}

// Wraps `stream` only when this exchange is allowed to decode (the client
// itself injected Accept-Encoding); otherwise hands the pointer back unchanged.
inline std::shared_ptr<ClientStream> maybe_decompressing_stream(std::shared_ptr<ClientStream> stream,
                                                                ResponseDecoding policy) {
    if (!policy.enabled || policy.encodings.empty()) {
        return stream;
    }
    return std::make_shared<DecompressingClientStream>(std::move(stream), std::move(policy));
}

} // namespace simple_http::detail
