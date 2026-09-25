#pragma once

// HttpClient: the outbound client facade.
//
// Two levels, one implementation:
//
//   convenience  co_await client.get(url) / post(url, body) / head / put / del
//                One call, one aggregated response. The body is bounded by
//                RequestOptions::max_body_bytes (defaulting to
//                ClientConfig::limits.max_body_bytes) so a peer cannot make the
//                process buffer forever, and each read is bounded by
//                ClientConfig::request_timeout.
//
//   session      co_await client.connect(target)
//                co_await session->open_stream(spec)
//                A connection the caller drives itself: streaming bodies both
//                ways, and — on HTTP/2 — as many concurrent streams as the peer
//                allows. Nothing is buffered that the caller did not ask for.
//
// Which HTTP version a connection speaks is negotiated, never guessed:
//   * https  — ALPN. Auto offers {"h2","http/1.1"} and takes what the peer
//              picks; Http2 requires "h2"; Http11 offers only "http/1.1".
//   * http   — Auto/Http2 use h2c per H2cMode: Upgrade (one request doubles as
//              the protocol switch, and a peer that ignores it just answers
//              HTTP/1.1) or PriorKnowledge (preface immediately, no round trip,
//              only for a peer known to speak h2c). Http11 stays HTTP/1.1.
// A policy that cannot be met is an error, not a silent downgrade:
// version_not_negotiated.
//
// Concurrency: every operation runs on the caller's executor (sessions are bound
// to it, model A), so this class holds no executor of its own — but one
// HttpClient may be shared across threads, since the pool is locked and sessions
// only ever hop onto their own executor.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>

#include "../core/limits.h"
#include "../core/logging.h"
#include "../core/types.h"
#include "../transport/tcp_transport.h"
#include "../transport/tls_transport.h"
#include "client_config.h"
#include "client_pool.h"
#include "client_stream.h"
#include "h1_client.h"
#include "h2_client.h"
#include "tls_client.h"
#include "url.h"

namespace simple_http {

namespace asio = boost::asio;

// A whole response, as the convenience level returns it.
struct ClientResponse {
    int status{0};
    Version version{Version::Http11};
    Headers headers;
    std::string body;
    // True when nothing followed the head (HEAD, 204/304).
    bool bodyless{false};

    std::optional<std::string_view> header(std::string_view name) const {
        return headers.get(name);
    }

    // 2xx.
    bool ok() const {
        return status >= 200 && status < 300;
    }
};

// Per-request knobs for the convenience level.
struct RequestOptions {
    // Cap on the response body this call will buffer. 0 = use
    // ClientConfig::limits.max_body_bytes.
    std::size_t max_body_bytes{0};
    // Per-operation budget: each read/write of the exchange gives up if the peer
    // says nothing for this long. 0 = use ClientConfig::request_timeout; a
    // negative value disables it.
    std::chrono::milliseconds timeout{0};
};

// An opened exchange, plus where its connection came from.
struct OpenedStream {
    std::shared_ptr<ClientStream> stream;
    // True when the connection came out of the pool. It sat idle, so the peer may
    // have closed it without either side noticing, and a transport failure before
    // any response byte is then replayable — a caller with nothing to lose (no
    // request body, nothing read from its own peer) may retry once on a fresh
    // connection. This is the rule the convenience layer applies internally; it
    // is exposed so a streaming caller (the reverse proxy) can apply it too.
    bool pooled{false};
};

// Connection counters, for diagnostics and for tests that assert reuse.
struct ClientStats {
    std::size_t connections_opened{0};  // fresh TCP/TLS connections dialed
    std::size_t connections_reused{0};  // sessions taken from the pool
};

// Whether a failure came from the transport rather than from a decision the
// client itself made (a version that could not be negotiated, a protocol error,
// an oversized body, …). Only transport failures on a *pooled* connection are
// worth replaying: the peer was idle and closed under us, so nothing can have
// been acted on — which is not true of a request a live connection dropped
// mid-flight.
inline bool transport_failure(const error_code& ec) {
    return ec.category() != client_category() || ec == make_error_code(client_errc::session_closed);
}

// Wraps an awaitable in a deadline. The awaitable's own I/O is cancelled when
// the timer wins, which is why callers must also cancel the stream.
template <typename T>
asio::awaitable<std::expected<T, error_code>> await_with_deadline(asio::awaitable<T> op,
                                                                  std::chrono::milliseconds limit) {
    using namespace asio::experimental::awaitable_operators;
    if (limit.count() <= 0)
        co_return co_await std::move(op);

    auto deadline_op = [limit]() -> asio::awaitable<void> {
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(limit);
        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    };
    auto outcome = co_await (std::move(op) || deadline_op());
    if (auto* value = std::get_if<T>(&outcome))
        co_return std::move(*value);
    co_return std::unexpected{make_error_code(client_errc::request_timeout)};
}

class HttpClient {
  public:
    // Building the TLS context can fail (missing CA file, unusable client
    // certificate), and it does so here rather than on the first request —
    // configuration faults should surface at construction, like the server's.
    explicit HttpClient(ClientConfig config = {})
        : m_config(std::move(config)),
          m_pool(std::make_shared<ClientPool>(m_config.max_idle_per_target, m_config.idle_pool_ttl)),
          m_ssl_context(make_client_ssl_context(m_config.tls)) {
    }

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    ClientConfig& config() {
        return m_config;
    }

    const ClientConfig& config() const {
        return m_config;
    }

    ClientStats stats() const {
        return ClientStats{m_opened.load(std::memory_order_relaxed), m_pool->reused_count()};
    }

    std::size_t idle_connections() const {
        return m_pool->idle_count();
    }

    // Closes every idle pooled connection (in-flight ones are untouched).
    void close_idle() {
        m_pool->clear();
    }


    // A session plus where it came from.
    struct Acquired {
        std::shared_ptr<ClientSession> session;
        bool pooled{false};
    };

    // --- session level ---

    // Connects to `target` (reusing a pooled session when one is available) and
    // returns the session. The caller opens streams on it.
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> connect(ClientTarget target) {
        auto ex = co_await asio::this_coro::executor;
        auto acquired = co_await acquire(std::move(target), ex, /*allow_pool=*/true);
        if (!acquired)
            co_return std::unexpected{acquired.error()};
        co_return acquired->session;
    }

    // Same, from a URL ("https://host:port/path"); the version policy and h2c
    // mode come from ClientConfig (default_version / default_h2c).
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> connect(std::string_view url) {
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        co_return co_await connect(target_from_url(*parsed));
    }

    // Starts one exchange on a connection to `target`, dialing or reusing a
    // session as needed. A non-empty spec.target is used as-is; empty means the
    // origin's "/". The returned OpenedStream says whether the connection came
    // from the pool.
    asio::awaitable<std::expected<OpenedStream, error_code>> open_stream(ClientTarget target, RequestSpec spec) {
        co_return co_await start_exchange(std::move(target), std::move(spec), /*url_target=*/{});
    }

    // Same, with the target taken from a URL (including its path and query when
    // spec.target is empty).
    asio::awaitable<std::expected<OpenedStream, error_code>> open_stream(std::string_view url, RequestSpec spec) {
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        std::string fallback{parsed->target};
        co_return co_await start_exchange(target_from_url(*parsed), std::move(spec), std::move(fallback));
    }

    // --- convenience level ---

    asio::awaitable<std::expected<ClientResponse, error_code>> request(ClientTarget target,
                                                                       RequestSpec spec,
                                                                       RequestOptions options = {}) {
        co_return co_await do_request(std::move(target), std::move(spec), {}, options);
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> request(std::string_view url,
                                                                       RequestSpec spec,
                                                                       RequestOptions options = {}) {
        auto parsed = parse_url(url);
        if (!parsed)
            co_return std::unexpected{parsed.error()};
        std::string fallback{parsed->target};
        co_return co_await do_request(target_from_url(*parsed), std::move(spec), std::move(fallback), options);
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> get(std::string_view url, RequestOptions options = {}) {
        RequestSpec spec;
        spec.method = Method::Get;
        co_return co_await request(url, std::move(spec), options);
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> post(std::string_view url,
                                                                    std::string body,
                                                                    std::string content_type = "text/plain",
                                                                    RequestOptions options = {}) {
        RequestSpec spec;
        spec.method = Method::Post;
        spec.headers.add("content-type", std::move(content_type));
        spec.body = std::move(body);
        co_return co_await request(url, std::move(spec), options);
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> put(std::string_view url,
                                                                   std::string body,
                                                                   std::string content_type = "text/plain",
                                                                   RequestOptions options = {}) {
        RequestSpec spec;
        spec.method = Method::Put;
        spec.headers.add("content-type", std::move(content_type));
        spec.body = std::move(body);
        co_return co_await request(url, std::move(spec), options);
    }

    // HEAD: the response carries the headers a GET would produce and no body.
    asio::awaitable<std::expected<ClientResponse, error_code>> head(std::string_view url,
                                                                    RequestOptions options = {}) {
        RequestSpec spec;
        spec.method = Method::Head;
        co_return co_await request(url, std::move(spec), options);
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> del(std::string_view url,
                                                                   RequestOptions options = {}) {
        RequestSpec spec;
        spec.method = Method::Delete;
        co_return co_await request(url, std::move(spec), options);
    }

  private:
    // The ClientTarget a URL describes, with the client-wide defaults for the
    // knobs a URL cannot express.
    ClientTarget target_from_url(const Url& url) const {
        ClientTarget target = url.to_target();
        target.version = m_config.default_version;
        target.h2c = m_config.default_h2c;
        return target;
    }

    asio::awaitable<std::expected<ClientResponse, error_code>> do_request(ClientTarget target,
                                                                          RequestSpec spec,
                                                                          std::string url_target,
                                                                          RequestOptions options) {
        if (spec.target.empty())
            spec.target = url_target.empty() ? "/" : url_target;
        const std::size_t cap = options.max_body_bytes != 0 ? options.max_body_bytes : m_config.limits.max_body_bytes;
        const std::chrono::milliseconds limit =
            options.timeout.count() != 0 ? options.timeout : m_config.request_timeout;

        for (int attempt = 0; attempt < 2; ++attempt) {
            auto opened = co_await start_exchange(target, spec, url_target, /*fresh_only=*/attempt > 0);
            if (!opened)
                co_return std::unexpected{opened.error()};
            auto& stream = opened->stream;

            auto response = co_await read_exchange(stream, cap, limit);
            if (response)
                co_return std::move(*response);

            const error_code ec = response.error();
            stream->cancel();  // h2: reset the stream, keeping the connection; h1: close it

            // A pooled connection that died before answering may be replayed: it
            // was idle, so the request never reached anyone. A streamed body has
            // already been taken from the caller and cannot be sent again.
            if (attempt == 0 && opened->pooled && !spec.stream_body && transport_failure(ec)) {
                SIMPLE_HTTP_ERROR_LOG("client: pooled connection to {} died before answering ({}), retrying",
                                      target.authority(),
                                      ec.message());
                continue;
            }
            co_return std::unexpected{ec};
        }
        co_return std::unexpected{make_error_code(client_errc::session_closed)};
    }

    // Reads the response of one exchange: head, then whole body, both under the
    // request budget.
    asio::awaitable<std::expected<ClientResponse, error_code>> read_exchange(std::shared_ptr<ClientStream> stream,
                                                                             std::size_t cap,
                                                                             std::chrono::milliseconds limit) {
        // Both expected layers matter: the outer one is the deadline, the inner
        // is the exchange itself.
        auto head = co_await await_with_deadline(stream->read_head(), limit);
        if (!head || !*head)
            co_return std::unexpected{head ? (*head).error() : head.error()};

        std::string body;
        for (;;) {
            auto chunk = co_await await_with_deadline(stream->read(), limit);
            if (!chunk || !*chunk)
                co_return std::unexpected{chunk ? (*chunk).error() : chunk.error()};
            if ((*chunk)->eof) break;
            if (cap != 0 && body.size() + (*chunk)->data.size() > cap)
                co_return std::unexpected{make_error_code(client_errc::body_too_large)};
            body.append((*chunk)->data);
        }
        co_return ClientResponse{
            (*head)->status, (*head)->version, std::move((*head)->headers), std::move(body), (*head)->bodyless};
    }

    // Opens a stream on a connection to `target`: a pooled session if one is
    // idle, otherwise a freshly dialed one. A pooled session that turns out to be
    // stale (the peer closed it while it sat idle) costs one retry on a fresh
    // connection — the same rule the server's reverse proxy uses, and safe here
    // because nothing has been written yet when it fails.
    asio::awaitable<std::expected<OpenedStream, error_code>> start_exchange(ClientTarget target,
                                                                      RequestSpec spec,
                                                                      std::string url_target,
                                                                      bool fresh_only = false) {
        if (spec.target.empty())
            spec.target = url_target.empty() ? "/" : url_target;

        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool allow_pool = !fresh_only && attempt == 0;
            auto acquired = co_await acquire(target, co_await asio::this_coro::executor, allow_pool);
            if (!acquired)
                co_return std::unexpected{acquired.error()};

            auto usable = co_await negotiate_and_open(acquired->session, target, spec);
            if (usable)
                co_return OpenedStream{*usable, acquired->pooled};

            // A pooled connection that had gone away fails as a transport error
            // on the first write or read; that is worth one try on a fresh
            // connection. A failure the peer decided (a version it will not
            // speak, a malformed reply) is reported as it is.
            const error_code ec = usable.error();
            if (acquired->pooled && transport_failure(ec)) {
                SIMPLE_HTTP_ERROR_LOG("client: {} dropped a pooled connection ({}), retrying on a fresh one",
                                      target.authority(),
                                      ec.message());
                acquired->session->close();
                continue;
            }
            co_return std::unexpected{ec};
        }
        co_return std::unexpected{make_error_code(client_errc::session_closed)};
    }

    // Opens a stream, performing the h2c upgrade when the target asks for it.
    asio::awaitable<std::expected<std::shared_ptr<ClientStream>, error_code>> negotiate_and_open(
        const std::shared_ptr<ClientSession>& session,
        const ClientTarget& target,
        RequestSpec spec) {
        const bool want_upgrade = !target.use_tls && target.h2c == H2cMode::Upgrade &&
                                  target.version != HttpVersionPolicy::Http11 && !spec.stream_body;
        if (want_upgrade) {
            auto h1 = std::dynamic_pointer_cast<Http1ClientSession<TcpStreamTransport>>(session);
            if (h1 && h1->upgrade_available(spec.stream_body)) {
                auto stream = co_await h1->open_stream_upgradeable(spec, h2_settings_base64url(m_config.limits));
                if (!stream)
                    co_return std::unexpected{stream.error()};
                // A pinned HTTP/2 policy is not satisfied by an ignored upgrade.
                if (target.version == HttpVersionPolicy::Http2 && (*stream)->version() != Version::Http2) {
                    (*stream)->cancel();
                    co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
                }
                co_return *stream;
            }
        }
        co_return co_await session->open_stream(std::move(spec));
    }

    // Takes a session for `target` from the pool, or dials a new one. The
    // `pooled` flag travels with it: only a pooled connection's failure is safe
    // to replay.
    asio::awaitable<std::expected<Acquired, error_code>> acquire(ClientTarget target,
                                                                 asio::any_io_executor ex,
                                                                 bool allow_pool) {
        const PoolKey key = make_key(target, ex);
        if (allow_pool) {
            if (auto session = m_pool->take(key))
                co_return Acquired{std::move(session), true};
        }
        auto session = co_await dial(target, std::move(ex), key);
        if (!session)
            co_return std::unexpected{session.error()};
        co_return Acquired{std::move(*session), false};
    }

    PoolKey make_key(const ClientTarget& target, const asio::any_io_executor& ex) const {
        PoolKey key;
        key.executor = static_cast<const void*>(&ex.context());
        key.origin = (target.use_tls ? "https://" : "http://") + target.authority();
        key.version_policy = static_cast<int>(target.version);
        key.tag = target.pool_tag;
        return key;
    }

    // Dials (resolving, connecting, handshaking, negotiating) and starts the
    // session. The new session is not pooled until its first exchange completes —
    // that is, until it is known to be at a request boundary.
    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> dial(ClientTarget target,
                                                                                    asio::any_io_executor ex,
                                                                                    PoolKey key) {
        error_code ec;
        auto endpoints = co_await resolve(target, ex);
        if (!endpoints)
            co_return std::unexpected{endpoints.error()};

        auto socket = std::make_shared<asio::ip::tcp::socket>(ex);
        apply_socket_options(*socket);
        if (auto cec = co_await connect_with_deadline(target.authority(), *socket, *endpoints)) {
            socket->close();
            co_return std::unexpected{cec};
        }
        error_code pe;
        auto peer = socket->remote_endpoint(pe);
        ++m_opened;

        if (target.use_tls) {
            co_return co_await start_tls_session(std::move(target), std::move(socket), peer, std::move(key));
        }
        co_return co_await start_plain_session(std::move(target), std::move(socket), peer, std::move(key));
    }

    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> start_plain_session(
        ClientTarget target,
        std::shared_ptr<asio::ip::tcp::socket> socket,
        asio::ip::tcp::endpoint peer,
        PoolKey key) {
        auto transport = std::make_shared<TcpStreamTransport>(std::move(socket), peer);
        // Plaintext h2c has two shapes, and which one applies is H2cMode's call:
        // PriorKnowledge speaks h2 from the first byte, while Upgrade stays
        // HTTP/1.1 until a request switches the connection — even when HTTP/2 is
        // required, in which case a peer that will not upgrade fails the request
        // rather than quietly answering over HTTP/1.1.
        const bool speak_h2_at_once =
            target.h2c == H2cMode::PriorKnowledge && target.version != HttpVersionPolicy::Http11;
        if (speak_h2_at_once) {
            auto session = std::make_shared<Http2ClientSession<TcpStreamTransport>>(transport,
                                                                                    target,
                                                                                    m_config.limits,
                                                                                    m_config.idle_timeout);
            wire_session(session, key);
            if (auto ec = co_await session->start(); ec) {
                session->close();
                co_return std::unexpected{ec};
            }
            co_return session;
        }
        auto session = std::make_shared<Http1ClientSession<TcpStreamTransport>>(transport,
                                                                                target.authority(),
                                                                                m_config.limits,
                                                                                m_config.idle_timeout);
        wire_h1_session(session, target, key);
        co_return session;
    }

    asio::awaitable<std::expected<std::shared_ptr<ClientSession>, error_code>> start_tls_session(
        ClientTarget target,
        std::shared_ptr<asio::ip::tcp::socket> socket,
        asio::ip::tcp::endpoint peer,
        PoolKey key) {
        auto stream = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(std::move(*socket), *m_ssl_context);
        auto transport = std::make_shared<TlsStreamTransport>(stream, peer);

        ClientTlsHandshake hs;
        hs.sni = target.sni.empty() && !m_config.tls.sni_override.empty() ? m_config.tls.sni_override
                                                                          : std::string{target.sni_host()};
        hs.verify_host = m_config.tls.verify_host && m_config.tls.verify_peer;
        hs.alpn_wire = alpn_wire_list(target, m_config.tls);

        // The handshake shares the connect budget: DNS + TCP + TLS is what the
        // caller's timeout is about.
        auto outcome = co_await await_with_deadline(tls_client_handshake(*transport, hs), m_config.connect_timeout);
        if (!outcome) {
            transport->close();
            co_return std::unexpected{outcome.error()};
        }
        if (error_code ec = *outcome) {
            SIMPLE_HTTP_ERROR_LOG("client: TLS handshake with {} failed: {}", target.sni_host(), ec.message());
            transport->close();
            co_return std::unexpected{ec};
        }

        const std::string_view alpn = transport->alpn_selected();
        if (target.version == HttpVersionPolicy::Http2 && alpn != "h2") {
            SIMPLE_HTTP_ERROR_LOG("client: {} did not negotiate HTTP/2 over TLS (ALPN='{}')", target.sni_host(), alpn);
            transport->close();
            co_return std::unexpected{make_error_code(client_errc::version_not_negotiated)};
        }
        if (alpn == "h2") {
            auto session = std::make_shared<Http2ClientSession<TlsStreamTransport>>(transport,
                                                                                    target,
                                                                                    m_config.limits,
                                                                                    m_config.idle_timeout);
            wire_session(session, key);
            if (auto ec = co_await session->start(); ec) {
                session->close();
                co_return std::unexpected{ec};
            }
            co_return session;
        }
        // "http/1.1", or no ALPN at all (an older peer): HTTP/1.1.
        auto session = std::make_shared<Http1ClientSession<TlsStreamTransport>>(transport,
                                                                                target.authority(),
                                                                                m_config.limits,
                                                                                m_config.idle_timeout);
        wire_h1_session(session, target, key);
        co_return session;
    }

    // Gives a session its pool identity: when it goes idle and reusable, it goes
    // back to the pool (the pool arms the idle timer that eventually closes it).
    template <typename Session>
    void wire_session(std::shared_ptr<Session> session, const PoolKey& key) {
        auto pool = m_pool;
        // Weak: the pool holds the strong reference, and a callback holding it
        // too would close a cycle the session could never escape.
        std::weak_ptr<Session> weak = session;
        session->set_on_idle([pool, key, weak] {
            if (auto live = weak.lock())
                pool->put(key, live);
        });
    }

    // The same, for an HTTP/1.1 session that may upgrade to h2c: the pool holds
    // this session, the successor reports its idleness through it, and a
    // successful upgrade hands the connection to an HTTP/2 session that is
    // started with the upgrading request as stream 1 (RFC 9113 §3.2).
    template <typename Session>
    void wire_h1_session(std::shared_ptr<Session> session, const ClientTarget& target, const PoolKey& key) {
        wire_session(session, key);
        using Transport = std::decay_t<decltype(*session->transport())>;
        auto config = m_config;
        auto pool = m_pool;
        std::weak_ptr<Session> weak_h1 = session;
        session->set_h2_upgrade_factory(
            [config, pool, target, key, weak_h1](std::shared_ptr<Transport> transport,
                                                 RequestSpec seed,
                                                 std::string initial)
                -> asio::awaitable<
                    std::expected<std::pair<std::shared_ptr<ClientSession>, std::shared_ptr<ClientStream>>,
                                  error_code>> {
                (void)seed;  // the peer already has the request: only stream 1 is recorded
                auto h2 = std::make_shared<Http2ClientSession<Transport>>(transport,
                                                                          target,
                                                                          config.limits,
                                                                          config.idle_timeout);
                if (auto h1 = weak_h1.lock()) {
                    // The pooled handle is the h1 session (it fronts the HTTP/2
                    // connection and reports its idleness), so the successor must
                    // own it: nothing else does between the upgrade and the first
                    // time the pool takes it.
                    h2->set_on_idle([h1] { h1->notify_idle(); });
                } else {
                    h2->set_on_idle([pool, key, h2] { pool->put(key, h2); });
                }
                auto stream = co_await h2->start_with_stream(RequestSpec{}, std::move(initial));
                if (!stream) {
                    h2->close();
                    co_return std::unexpected{stream.error()};
                }
                co_return std::make_pair(std::shared_ptr<ClientSession>{h2}, *stream);
            });
    }

    asio::awaitable<std::expected<std::vector<asio::ip::tcp::endpoint>, error_code>> resolve(
        const ClientTarget& target,
        const asio::any_io_executor& ex) {
        if (m_config.resolve) {
            auto [ec, endpoints] = co_await m_config.resolve(target.host, std::to_string(target.effective_port()));
            if (ec)
                co_return std::unexpected{ec};
            if (endpoints.empty())
                co_return std::unexpected{make_error_code(asio::error::host_not_found)};
            co_return endpoints;
        }
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(ex);
        auto [ec, results] = co_await resolver->async_resolve(target.host,
                                                              std::to_string(target.effective_port()),
                                                              asio::as_tuple(asio::use_awaitable));
        if (ec)
            co_return std::unexpected{ec};
        std::vector<asio::ip::tcp::endpoint> endpoints;
        endpoints.reserve(results.size());
        for (const auto& entry : results)
            endpoints.push_back(entry.endpoint());
        co_return endpoints;
    }

    asio::awaitable<error_code> connect_with_deadline(const std::string& authority,
                                                      asio::ip::tcp::socket& socket,
                                                      const std::vector<asio::ip::tcp::endpoint>& endpoints) {
        using namespace asio::experimental::awaitable_operators;
        auto connect_op = [&socket, &endpoints]() -> asio::awaitable<std::tuple<error_code, asio::ip::tcp::endpoint>> {
            co_return co_await asio::async_connect(socket, endpoints, asio::as_tuple(asio::use_awaitable));
        };
        if (m_config.connect_timeout.count() <= 0) {
            auto [ec, endpoint] = co_await connect_op();
            (void)endpoint;
            co_return ec;
        }
        auto deadline_op = [this]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(m_config.connect_timeout);
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        };
        auto outcome = co_await (connect_op() || deadline_op());
        if (auto* result = std::get_if<std::tuple<error_code, asio::ip::tcp::endpoint>>(&outcome)) {
            co_return std::get<0>(*result);
        }
        SIMPLE_HTTP_ERROR_LOG("client: connecting to {} timed out after {}ms",
                              authority,
                              m_config.connect_timeout.count());
        (void)authority;  // only reported through the log, which may be compiled out
        co_return make_error_code(client_errc::connect_timeout);
    }

    void apply_socket_options(asio::ip::tcp::socket& socket) {
        error_code ec;
        if (m_config.tcp_nodelay)
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
        if (m_config.tcp_keepalive)
            socket.set_option(asio::socket_base::keep_alive(true), ec);
        if (m_config.socket_setup) {
            try {
                m_config.socket_setup(socket);
            } catch (const std::exception& e) {
                SIMPLE_HTTP_ERROR_LOG("client: socket_setup threw: {}", e.what());
            }
        }
    }

    ClientConfig m_config;
    std::shared_ptr<ClientPool> m_pool;
    std::shared_ptr<asio::ssl::context> m_ssl_context;  // outlives every stream using it
    std::atomic<std::size_t> m_opened{0};  // an HttpClient may be shared across threads
};

}  // namespace simple_http
