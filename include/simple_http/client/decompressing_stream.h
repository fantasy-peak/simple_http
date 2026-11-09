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
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>

#include "../core/content_encoding.h"
#include "../core/http_status.h"
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

class DecompressingClientStream final : public ClientStream,
                                         public std::enable_shared_from_this<DecompressingClientStream> {
  public:
    DecompressingClientStream(std::shared_ptr<ClientStream> inner, ResponseDecoding policy,
                              std::size_t max_decoded_bytes = 0)
        : m_inner(std::move(inner)), m_policy(std::move(policy)), m_max_decoded_bytes(max_decoded_bytes) {}

    void write(std::string data, WriteCompletion done) override { m_inner->write(std::move(data), std::move(done)); }
    void finish(std::string data, WriteCompletion done) override { m_inner->finish(std::move(data), std::move(done)); }
    void finish_trailers(Headers trailers, WriteCompletion done) override {
        m_inner->finish_trailers(std::move(trailers), std::move(done));
    }
    void cancel(WriteCompletion done) override { m_inner->cancel(std::move(done)); }

    void read(ReadCompletion done) override {
        auto completion = std::make_shared<ReadCompletion>(std::move(done));
        auto self = shared_from_this();
        // Carry the caller's slot through the internal head read too: the
        // composed head operation takes its cancellation state from this token,
        // and await_head() forwards it to the inner stream.
        auto slot = asio::get_associated_cancellation_slot(*completion);
        auto handler = [self, completion](error_code hec, ResponseHead) mutable {
            if (hec) {
                auto callback = std::move(*completion);
                std::move(callback)(hec, ReadResult{});
                return;
            }
            self->read_impl(std::move(completion));
        };
        if (slot.is_connected()) {
            async_read_head(asio::bind_cancellation_slot(slot, std::move(handler)));
        } else {
            async_read_head(std::move(handler));
        }
    }

    void clear_head_cache() override {
        reset_head();
        m_inner->clear_head_cache();
    }

    bool finished() const override { return m_inner->finished(); }
    [[nodiscard]] const Headers &response_trailers() const noexcept override { return m_inner->response_trailers(); }
    Version version() const override { return m_inner->version(); }
    std::uint32_t id() const override { return m_inner->id(); }

    void await_head(WriteCompletion done) override {
        auto self = shared_from_this();
        // The forwarding lambda below has no associators of its own. Preserve
        // the caller's cancellation slot explicitly, or Stream's head deadline
        // cannot abort the inner read (and the awaitable hangs forever).
        auto slot = asio::get_associated_cancellation_slot(done);
        auto handler = [self, done = std::move(done)](error_code ec, ResponseHead head) mutable {
            if (ec) {
                std::move(done)(ec);
                return;
            }
            if (!head.bodyless) {
                const auto values = head.headers.get_all("content-encoding");
                if (values.size() == 1) {
                    const auto coding = parse_content_coding(values.front());
                    std::string canonical;
                    if (coding == ContentCoding::Gzip) {
                        canonical = "gzip";
                    } else if (coding == ContentCoding::Brotli) {
                        canonical = "br";
                    }
                    const bool advertised = !canonical.empty() &&
                                            std::find(self->m_policy.encodings.begin(), self->m_policy.encodings.end(),
                                                      canonical) != self->m_policy.encodings.end();
                    if (advertised) {
                        self->m_decoder = make_decoder(canonical);
                        if (self->m_decoder) {
                            head.headers.erase("content-encoding");
                            head.headers.erase("content-length");
                            head.uncompressed = true;
                        }
                    }
                }
            }
            const bool interim = head.status == status::continue_;
            self->set_head(std::move(head));
            if (interim) {
                self->reset_head();
            }
            std::move(done)(error_code{});
        };
        if (slot.is_connected()) {
            m_inner->async_read_head(asio::bind_cancellation_slot(slot, std::move(handler)));
        } else {
            m_inner->async_read_head(std::move(handler));
        }
    }

    asio::any_io_executor head_executor() const override { return m_inner->head_executor(); }

  private:
    // Forward to the inner stream while preserving the caller's cancellation
    // slot. A plain lambda would drop it, so Stream's body-idle deadline could
    // never abort a parked inner read.
    template <typename Handler> void forward_read(const std::shared_ptr<ReadCompletion> &completion, Handler &&handler) {
        auto slot = asio::get_associated_cancellation_slot(*completion);
        if (slot.is_connected()) {
            m_inner->read(asio::bind_cancellation_slot(slot, std::forward<Handler>(handler)));
        } else {
            m_inner->read(std::forward<Handler>(handler));
        }
    }

    void read_impl(std::shared_ptr<ReadCompletion> completion) {
        if (!m_decoder) {
            forward_read(completion, [completion](error_code ec, ReadResult result) mutable {
                auto callback = std::move(*completion);
                std::move(callback)(ec, std::move(result));
            });
            return;
        }
        if (!m_tail.empty()) {
            std::string out = std::move(m_tail);
            m_tail.clear();
            auto callback = std::move(*completion);
            std::move(callback)(error_code{}, ReadResult{std::move(out), false});
            return;
        }
        if (m_inner_eof) {
            auto callback = std::move(*completion);
            std::move(callback)(error_code{}, ReadResult{std::string{}, true});
            return;
        }
        auto self = shared_from_this();
        forward_read(completion, [self, completion](error_code ec, ReadResult chunk) mutable {
            if (ec) {
                auto callback = std::move(*completion);
                std::move(callback)(ec, ReadResult{});
                return;
            }
            if (chunk.eof) {
                self->m_inner_eof = true;
                bool limited = false;
                self->m_tail = self->m_decoder->finish_bounded(self->remaining_decoded_room(), limited);
                if (limited) {
                    auto callback = std::move(*completion);
                    std::move(callback)(make_error_code(client_errc::body_too_large), ReadResult{});
                    return;
                }
                self->m_decoded_total += self->m_tail.size();
                if (self->m_decoder->failed()) {
                    auto callback = std::move(*completion);
                    std::move(callback)(make_error_code(client_errc::body_decode_failed), ReadResult{});
                    return;
                }
                if (!self->m_tail.empty()) {
                    std::string out = std::move(self->m_tail);
                    self->m_tail.clear();
                    auto callback = std::move(*completion);
                    std::move(callback)(error_code{}, ReadResult{std::move(out), false});
                    return;
                }
                auto callback = std::move(*completion);
                std::move(callback)(error_code{}, ReadResult{std::string{}, true});
                return;
            }
            bool limited = false;
            std::string out = self->m_decoder->write_bounded(chunk.data, self->remaining_decoded_room(), limited);
            if (limited) {
                auto callback = std::move(*completion);
                std::move(callback)(make_error_code(client_errc::body_too_large), ReadResult{});
                return;
            }
            self->m_decoded_total += out.size();
            if (self->m_decoder->failed()) {
                auto callback = std::move(*completion);
                std::move(callback)(make_error_code(client_errc::body_decode_failed), ReadResult{});
                return;
            }
            if (!out.empty()) {
                auto callback = std::move(*completion);
                std::move(callback)(error_code{}, ReadResult{std::move(out), false});
                return;
            }
            self->read_impl(std::move(completion));
        });
    }

    std::size_t remaining_decoded_room() const {
        if (m_max_decoded_bytes == 0)
            return std::numeric_limits<std::size_t>::max();
        return m_decoded_total >= m_max_decoded_bytes ? 0 : m_max_decoded_bytes - m_decoded_total;
    }

    std::shared_ptr<ClientStream> m_inner;
    ResponseDecoding m_policy;
    std::unique_ptr<ContentDecoder> m_decoder;
    std::string m_tail;
    bool m_inner_eof{false};
    std::size_t m_max_decoded_bytes{0};
    std::size_t m_decoded_total{0};
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
                                                                ResponseDecoding policy,
                                                                std::size_t max_decoded_bytes = 0) {
    if (!policy.enabled || policy.encodings.empty()) {
        return stream;
    }
    return std::make_shared<DecompressingClientStream>(std::move(stream), std::move(policy), max_decoded_bytes);
}

} // namespace simple_http::detail
