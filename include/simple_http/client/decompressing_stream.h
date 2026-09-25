#pragma once

// DecompressingClientStream: a ClientStream decorator that decodes the response
// body when it arrives with a Content-Encoding this build can handle.
//
// Decoding happens in read(), not in the convenience layer, so every caller sees
// the original bytes - and, usefully, so do the size caps: read_all()
// (client_stream.h) and the aggregate in HttpClient::read_exchange both bound
// what read() returns, which is now the *decoded* size. A compression bomb
// cannot slip past them by being small on the wire.
//
// The head is rewritten as well. Once the body is decoded it is no longer the
// encoded representation, so Content-Encoding is dropped; Content-Length no
// longer describes what follows, so it goes too. The rewrite is published with
// set_head() so the non-virtual head()/status()/read_head() report it.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "../core/content_encoding.h"
#include "../core/types.h"
#include "client_config.h"
#include "client_stream.h"

namespace simple_http {

namespace asio = boost::asio;

class DecompressingClientStream final : public ClientStream {
  public:
    explicit DecompressingClientStream(std::shared_ptr<ClientStream> inner) : m_inner(std::move(inner)) {}

    // --- request side: nothing to decode on the way out ---
    asio::awaitable<error_code> write(std::string data) override { return m_inner->write(std::move(data)); }
    asio::awaitable<error_code> finish(std::string data) override { return m_inner->finish(std::move(data)); }

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

    bool finished() const override { return m_inner->finished(); }
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
        if (auto encoding = head->headers.get("content-encoding"); encoding && *encoding != kEncodingIdentity) {
            m_decoder = make_decoder(*encoding);
            if (m_decoder) {
                head->headers.erase("content-encoding");
                head->headers.erase("content-length");
            }
            // No decoder (an encoding we cannot handle): the body is passed
            // through untouched and the head is left alone, so the caller can
            // still see what it is looking at.
        }
        set_head(std::move(*head));
        co_return error_code{};
    }

  private:
    std::shared_ptr<ClientStream> m_inner;
    std::unique_ptr<ContentDecoder> m_decoder;
    std::string m_tail;         // decoded bytes still owed to the caller
    bool m_inner_eof{false};    // the inner stream has reported end-of-body
};

// Joins the configured encodings, keeping only those this build can actually
// decode: never advertise something we would then have to pass through raw.
inline std::string accept_encoding_value(const std::vector<std::string>& wanted) {
    std::string out;
    for (const std::string& encoding : wanted) {
        if (!make_decoder(encoding)) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += encoding;
    }
    return out;
}

// Wraps `stream` only when the caller asked for automatic decoding; otherwise
// hands the pointer back unchanged - no allocation, no behaviour change.
inline std::shared_ptr<ClientStream> maybe_decompressing_stream(std::shared_ptr<ClientStream> stream,
                                                                bool auto_decompress) {
    if (!auto_decompress) {
        return stream;
    }
    return std::make_shared<DecompressingClientStream>(std::move(stream));
}

}  // namespace simple_http
