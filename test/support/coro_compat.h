#pragma once

// Coroutine-side adapters for the token library, used by the external
// differential drivers and probes.
//
// The library's asynchronous API is completion-token based. Its own tests are
// C++23 coroutines, so they await those operations with
// `asio::as_tuple(asio::use_awaitable)`; these helpers wrap the frequent shapes
// back into the value-or-error (`std::expected`) model the reference drivers
// were written against. Nothing here is part of the library.

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <expected>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "simple_http.h"

namespace simple_http::test {

namespace asio = boost::asio;

template <typename T>
asio::awaitable<std::expected<T, error_code>> as_expected(asio::awaitable<std::tuple<error_code, T>> op) {
    auto [ec, value] = co_await std::move(op);
    if (ec)
        co_return std::unexpected{ec};
    co_return std::move(value);
}

inline asio::awaitable<std::expected<void, error_code>> as_expected_void(asio::awaitable<std::tuple<error_code>> op) {
    auto [ec] = co_await std::move(op);
    if (ec)
        co_return std::unexpected{ec};
    co_return std::expected<void, error_code>{};
}

inline asio::awaitable<error_code> as_ec(asio::awaitable<std::tuple<error_code>> op) {
    auto [ec] = co_await std::move(op);
    co_return ec;
}

// --- server side -------------------------------------------------------------

inline asio::awaitable<std::expected<std::string, error_code>> body_read_all(Body &body) {
    return as_expected(body.async_read_all(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<ReadResult, error_code>> body_read(Body &body) {
    return as_expected(body.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<WsMessage, error_code>> ws_read(WebSocket &ws) {
    return as_expected(ws.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write(WebSocket &ws, std::string data, bool text) {
    return as_ec(ws.async_write(std::move(data), text, asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write_view(WebSocket &ws, std::string_view data, bool text) {
    return as_ec(ws.async_write_view(data, text, asio::as_tuple(asio::use_awaitable)));
}

// --- client side -------------------------------------------------------------

template <typename Builder>
asio::awaitable<std::expected<Response, error_code>> client_send(Builder &&builder) {
    auto [ec, response] = co_await builder.send(asio::as_tuple(asio::use_awaitable));
    if (ec)
        co_return std::unexpected{ec};
    co_return std::move(response);
}

inline asio::awaitable<std::expected<std::shared_ptr<Stream>, error_code>>
client_open_stream(Client &client, std::string url, StreamSpec spec = {}, std::size_t max_body_bytes = 0) {
    auto [ec, stream] = co_await client.async_open_stream(std::move(url), std::move(spec), max_body_bytes,
                                                          asio::as_tuple(asio::use_awaitable));
    if (ec)
        co_return std::unexpected{ec};
    co_return stream;
}

inline asio::awaitable<error_code> stream_write(Stream &stream, std::string data) {
    return as_ec(stream.async_write(std::move(data), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> stream_finish(Stream &stream, std::string last = {}) {
    return as_ec(stream.async_finish(std::move(last), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> stream_finish_trailers(Stream &stream, Headers trailers) {
    return as_ec(stream.async_finish_trailers(std::move(trailers), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<detail::ResponseHead, error_code>> stream_read_head(Stream &stream) {
    return as_expected(stream.async_read_head(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<ReadResult, error_code>> stream_read(Stream &stream) {
    return as_expected(stream.async_read(asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<std::expected<std::string, error_code>> stream_read_all(Stream &stream,
                                                                              std::size_t max_bytes = 0) {
    return as_expected(stream.async_read_all(max_bytes, asio::as_tuple(asio::use_awaitable)));
}

// --- multipart / SSE writers -------------------------------------------------

inline asio::awaitable<std::expected<std::shared_ptr<MultipartStreamWriter>, error_code>>
client_open_multipart(Client &client, std::string url, MultipartStreamOptions options) {
    auto [ec, writer] = co_await MultipartStreamWriter::async_open(client, std::move(url), std::move(options),
                                                                   asio::as_tuple(asio::use_awaitable));
    if (ec)
        co_return std::unexpected{ec};
    co_return writer;
}

inline asio::awaitable<error_code> mp_field(MultipartStreamWriter &writer, std::string name, std::string value) {
    return as_ec(writer.async_field(std::move(name), std::move(value), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> mp_file_data(MultipartStreamWriter &writer, std::string name, std::string filename,
                                               std::string content_type, std::string data) {
    return as_ec(writer.async_file_data(std::move(name), std::move(filename), std::move(content_type),
                                        std::move(data), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> mp_finish(MultipartStreamWriter &writer) {
    return as_ec(writer.async_finish(asio::as_tuple(asio::use_awaitable)));
}

// One spelling for "end this kind of stream" so the drivers do not have to
// care whether the handle in scope is a Stream or a MultipartStreamWriter.
inline asio::awaitable<error_code> finish_any(Stream &stream, std::string last = {}) {
    return stream_finish(stream, std::move(last));
}
inline asio::awaitable<error_code> finish_any(MultipartStreamWriter &writer) { return mp_finish(writer); }

inline asio::awaitable<std::expected<std::shared_ptr<SseStream>, error_code>>
client_open_sse(Client &client, std::string url, SseOptions options = {}) {
    auto [ec, sse] = co_await SseStream::async_open(client, std::move(url), std::move(options),
                                                   asio::as_tuple(asio::use_awaitable));
    if (ec)
        co_return std::unexpected{ec};
    co_return sse;
}

inline asio::awaitable<std::expected<SseEvent, error_code>> sse_next(SseStream &sse) {
    return as_expected(sse.async_next(asio::as_tuple(asio::use_awaitable)));
}

// --- client WebSocket --------------------------------------------------------

inline asio::awaitable<std::expected<std::shared_ptr<WebSocket>, error_code>>
client_open_websocket(Client &client, std::string url, WebSocketSpec spec = {}) {
    auto [ec, ws] = co_await client.async_open_websocket(std::move(url), std::move(spec),
                                                        asio::as_tuple(asio::use_awaitable));
    if (ec)
        co_return std::unexpected{ec};
    co_return ws;
}

inline asio::awaitable<error_code> ws_write_text(WebSocket &ws, std::string data) {
    return as_ec(ws.async_write_text(std::move(data), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_write_binary(WebSocket &ws, std::string data) {
    return as_ec(ws.async_write_binary(std::move(data), asio::as_tuple(asio::use_awaitable)));
}

inline asio::awaitable<error_code> ws_close(WebSocket &ws) {
    return as_ec(ws.async_close(asio::as_tuple(asio::use_awaitable)));
}

} // namespace simple_http::test
