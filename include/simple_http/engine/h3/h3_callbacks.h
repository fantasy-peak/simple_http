#pragma once

// The nghttp3 callback table, and the interface it delivers into.
//
// nghttp3 is a C library: every callback arrives with a `void* conn_user_data`
// and nothing else, has to be `noexcept` in effect (C cannot see a C++
// exception) and runs *synchronously inside* an nghttp3 call. That last property
// is the one that shapes this file: a callback may not call back into nghttp3,
// because it would re-enter the parser that is currently running.
//
// So the callbacks do no work. They unpack their arguments and hand them to an
// `H3CallbackSink`, which the HTTP/3 engine implements; anything that needs to
// await, allocate, or call nghttp3 happens later, on the executor, from a
// coroutine the sink spawned. The one exception is a read-only getter
// (`nghttp3_conn_get_frame_payload_left2`, used to enforce a limit nghttp3
// declines to enforce itself), which cannot re-enter anything.
//
// The indirection through an interface rather than a template is deliberate: it
// keeps the callback table a single non-template object, so there is one copy of
// each trampoline in the binary instead of one per connection type.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include <openssl/rand.h>

#include <nghttp3/nghttp3.h>

namespace simple_http::h3 {

// What the nghttp3 callbacks deliver to. Every method must be `noexcept` and
// must not call into nghttp3 — see the note at the top of this file.
class H3CallbackSink {
  public:
    H3CallbackSink() = default;
    virtual ~H3CallbackSink() = default;
    H3CallbackSink(const H3CallbackSink&) = delete;
    H3CallbackSink& operator=(const H3CallbackSink&) = delete;

    // --- request inbound ---------------------------------------------------

    // A field section (request headers, or trailers) is starting. Returning
    // false aborts the parse, which the engine uses to reject an oversized
    // section before nghttp3 buffers it.
    [[nodiscard]] virtual bool on_headers_begin(std::int64_t stream_id) noexcept = 0;
    // One field. `token` is nghttp3's classification of a pseudo-header; a
    // regular field carries the same name it would on the wire.
    virtual void on_header(std::int64_t stream_id, std::int32_t token, std::string_view name,
                           std::string_view value) noexcept = 0;
    // The field section ended. `fin` says the stream ends with it — for a
    // request that is a header-only request with no body.
    virtual void on_headers_end(std::int64_t stream_id, bool fin) noexcept = 0;
    // Request body bytes.
    virtual void on_data(std::int64_t stream_id, std::span<const std::uint8_t> data) noexcept = 0;
    // The request is complete: FIN and a well-formed message.
    virtual void on_end_stream(std::int64_t stream_id) noexcept = 0;

    // --- lifecycle and flow control ----------------------------------------

    virtual void on_stream_closed(std::int64_t stream_id, std::uint64_t app_error_code) noexcept = 0;
    virtual void on_acked(std::int64_t stream_id, std::uint64_t datalen) noexcept = 0;
    // The peer sent STOP_SENDING: it does not want the rest of our response.
    virtual void on_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) noexcept = 0;
    // nghttp3 wants the stream reset (it decides this, not the peer).
    virtual void on_reset_stream(std::int64_t stream_id, std::uint64_t app_error_code) noexcept = 0;
    // Bytes nghttp3 consumed while a field section was blocked on the QPACK
    // encoder stream. They were delivered earlier, so the flow-control credit
    // for them is owed now rather than then.
    virtual void on_deferred_consume(std::int64_t stream_id, std::size_t nconsumed) noexcept = 0;
    virtual void on_peer_settings(const nghttp3_proto_settings& settings) noexcept = 0;
    // The peer asked us to wind down (GOAWAY).
    virtual void on_shutdown(std::int64_t push_id) noexcept = 0;

    // --- response outbound --------------------------------------------------

    // nghttp3 asking for the next slice of a response body. Called at most once
    // per stream per `nghttp3_conn_writev_stream`, from inside the QUIC write
    // loop. Returns the number of `nghttp3_vec` filled, or
    // `NGHTTP3_ERR_WOULDBLOCK` when there is more body coming but none of it is
    // ready yet. Returning zero vectors *without* `NGHTTP3_DATA_FLAG_EOF` trips
    // an assertion inside nghttp3, so "not ready" must use the error code.
    virtual nghttp3_ssize read_response_data(std::int64_t stream_id, nghttp3_vec* vec, std::size_t veccnt,
                                             std::uint32_t* flags) noexcept = 0;
};

// The callback table. One static instance, filled by `h3_callbacks()`.
[[nodiscard]] inline const nghttp3_callbacks& h3_callbacks() {
    static const nghttp3_callbacks table = [] {
        nghttp3_callbacks cb{};
        cb.recv_data = [](nghttp3_conn*, std::int64_t stream_id, const std::uint8_t* data, std::size_t datalen,
                          void* conn_user_data, void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_data(
                stream_id, std::span<const std::uint8_t>{data, datalen});
            return 0;
        };
        cb.acked_stream_data = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t datalen, void* conn_user_data,
                                  void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_acked(stream_id, datalen);
            return 0;
        };
        cb.stream_close = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t app_error_code, void* conn_user_data,
                             void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_stream_closed(stream_id, app_error_code);
            return 0;
        };
        cb.deferred_consume = [](nghttp3_conn*, std::int64_t stream_id, std::size_t nconsumed, void* conn_user_data,
                                 void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_deferred_consume(stream_id, nconsumed);
            return 0;
        };
        cb.begin_headers = [](nghttp3_conn*, std::int64_t stream_id, void* conn_user_data, void*) -> int {
            if (!static_cast<H3CallbackSink*>(conn_user_data)->on_headers_begin(stream_id)) {
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
            return 0;
        };
        cb.recv_header = [](nghttp3_conn*, std::int64_t stream_id, std::int32_t token, nghttp3_rcbuf* name,
                            nghttp3_rcbuf* value, std::uint8_t, void* conn_user_data, void*) -> int {
            const nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
            const nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
            // The buffers are reference counted and owned by nghttp3; the sink
            // copies what it keeps.
            static_cast<H3CallbackSink*>(conn_user_data)->on_header(
                stream_id, token, std::string_view{reinterpret_cast<const char*>(n.base), n.len},
                std::string_view{reinterpret_cast<const char*>(v.base), v.len});
            return 0;
        };
        cb.end_headers = [](nghttp3_conn*, std::int64_t stream_id, int fin, void* conn_user_data, void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_headers_end(stream_id, fin != 0);
            return 0;
        };
        cb.end_stream = [](nghttp3_conn*, std::int64_t stream_id, void* conn_user_data, void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_end_stream(stream_id);
            return 0;
        };
        cb.stop_sending = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t app_error_code, void* conn_user_data,
                             void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_stop_sending(stream_id, app_error_code);
            return 0;
        };
        cb.reset_stream = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t app_error_code, void* conn_user_data,
                             void*) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_reset_stream(stream_id, app_error_code);
            return 0;
        };
        cb.shutdown = [](nghttp3_conn*, std::int64_t push_id, void* conn_user_data) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_shutdown(push_id);
            return 0;
        };
        cb.recv_settings2 = [](nghttp3_conn*, const nghttp3_proto_settings* settings, void* conn_user_data) -> int {
            static_cast<H3CallbackSink*>(conn_user_data)->on_peer_settings(*settings);
            return 0;
        };
        cb.rand = [](std::uint8_t* dest, std::size_t destlen) {
            // nghttp3's glitch rate limiter wants randomness, and it is a `void`
            // callback: there is no way to report a failure. A missing CSPRNG is
            // unrecoverable anyway.
            if (RAND_bytes(dest, static_cast<int>(destlen)) != 1) std::abort();
        };
        return cb;
    }();
    return table;
}

}  // namespace simple_http::h3
