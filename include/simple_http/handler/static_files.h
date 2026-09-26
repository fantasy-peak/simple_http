#pragma once

// Static file serving: the handler-layer half of a document root turned into a
// lookup table. core/static_table.h does the scanning, the security-relevant
// path handling and the representation choice; this half decides what to
// actually answer with, and is the only part that touches Request/Response.
//
// The contract callers depend on is narrow and worth stating twice:
//
//   * try_serve() returns true only after writing a **complete** response —
//     200/206/304/400/405/406/416. It never writes a head and leaves the body to
//     someone else.
//   * try_serve() returns false having written **nothing at all**, and always
//     means "this path is not mine". A miss therefore cannot become a 200: the
//     caller's own 404 handling takes over, and if the caller has none, it must
//     supply one (Router::static_files registers the built-in 404 behind the
//     stage, so a registered site cannot leave a request unanswered).
//
// Several header values below are set by hand rather than left to the engines,
// and each is load-bearing:
//
//   * HEAD must carry the Content-Length a GET would have produced. The engines
//     write headers only, and the one-shot send() would compute the length of
//     the empty body instead.
//   * An empty file needs an explicit `content-length: 0`, because HTTP/1.1
//     would frame it as chunked while HTTP/2 frames it as END_STREAM — two
//     different byte sequences for one file.
//   * A 304 must not carry Content-Length at all (RFC 9110 §15.4.5), so it goes
//     through send_bodyless().
//   * A 416 must *not* use send_bodyless(): only 204/304 and HEAD responses are
//     body-less by rule, so a 416 that omits Content-Length and has no
//     transfer-encoding is delimited by connection close in HTTP/1.1 — the
//     client then waits for the idle watchdog instead of reading a response that
//     was already complete. It sends an empty body with an explicit zero length.
//
// Interaction with response compression (CompressionConfig): safe, and for
// reasons that are worth knowing. The compressing writer declines a response
// that already carries `content-encoding` (a pre-compressed sibling passes
// through untouched), declines 204/304/206 and HEAD, and merges rather than
// duplicates `Vary`. The one case to understand is identity: with compression
// enabled globally, an identity representation may be compressed in flight
// while the pre-compressed siblings sit unused — correct, just not the cheapest
// path.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "../core/http_date.h"    // http_date
#include "../core/http_field.h"   // field::
#include "../core/http_status.h"  // status::
#include "../core/static_table.h"
#include "../core/url_path.h"  // decode_and_normalize, under_prefix
#include "../proto/request.h"
#include "../proto/response.h"
#include "handler.h"  // RequestPtr, ResponsePtr

namespace simple_http {

namespace asio = boost::asio;

struct StaticFilesConfig {
    // The table this server answers from. Nested rather than flattened so the
    // scan limits stay in one place and cannot be half-copied.
    StaticTableConfig table{};
    // Overrides the library's own `simple_http_server/x.y.z` Server header, which
    // names the exact software the server is built from. Empty keeps the library
    // default: a library should not claim to be something else by default, so a
    // deployment that wants a different name has to say so.
    std::string server_header{};
    // Path prefixes the site must never answer, even when a file exists under the
    // root at that path. Defense in depth: it is what stops a stray file from
    // shadowing a real route if registration order ever changes.
    std::vector<std::string> reserved_prefixes{};
    // Root-relative key to answer with when a GET/HEAD misses the table, which is
    // what a single-page app needs: its client-side router owns paths like
    // /user/123 that do not exist on disk. Empty disables the fallback entirely,
    // and then a miss is just a miss.
    std::string spa_fallback{};
    // Only answer a miss from spa_fallback when the path's last segment carries
    // no '.' — so /user/123 falls back but /missing.js cleanly 404s. A 200 with an
    // HTML body where a script was expected produces a MIME error in the console
    // that reads as a broken build rather than a missing file, which is
    // considerably harder to diagnose than a 404.
    bool spa_fallback_extensionless_only{true};
};

namespace detail {

// Applies the headers every response from this module carries, each exactly
// once. Response::header() and content_type() *append*, so a second call for the
// same name puts a duplicate on the wire — worse than omitting it.
//
// `etag` and `last_modified` are passed as optionals rather than derived here
// because the SPA fallback must omit them: see try_serve.
inline void apply_common_headers(const std::string& server_header, const ResponsePtr& res,
                                 const std::string& content_type, std::string_view cache_control,
                                 const std::optional<std::string>& etag, std::int64_t mtime) {
    if (!server_header.empty()) {
        res->header(field::server, server_header);
    }
    res->header(field::date, http_date(now_unix()));
    res->header(field::cache_control, std::string{cache_control});
    if (etag) res->header(field::etag, *etag);
    if (mtime > 0) res->header(field::last_modified, http_date(mtime));
    res->header(field::vary, "Accept-Encoding");
    res->content_type(content_type);
}

}  // namespace detail

class StaticFiles {
  public:
    using Stats = StaticTable::Stats;

    explicit StaticFiles(StaticFilesConfig config)
        : m_config(std::move(config)), m_table(m_config.table) {}

    // Forwards to the table. Must complete before the server serves anything.
    bool load(std::string& error) { return m_table.load(error); }

    bool enabled() const noexcept { return m_table.enabled(); }
    const Stats& stats() const noexcept { return m_table.stats(); }
    const std::string& root() const noexcept { return m_table.root(); }

    // Read-only table lookup, exposed for tests and for callers that want to
    // answer a miss their own way.
    const StaticEntry* find(std::string_view key) const { return m_table.find(key); }

    bool reserved(std::string_view normalized_path) const {
        for (const auto& p : m_config.reserved_prefixes) {
            if (under_prefix(normalized_path, p)) return true;
        }
        return false;
    }

    // Serves the request when it names something this table knows. Returns true
    // once a complete response has been written; false — with nothing written —
    // when the path is not ours, so the caller's 404 path takes it.
    asio::awaitable<bool> try_serve(const RequestPtr& req, const ResponsePtr& res) const;

    // The site's own 404 page when there is one, a plain-text body otherwise.
    // Always writes a response.
    asio::awaitable<void> serve_not_found(const RequestPtr& req, const ResponsePtr& res) const;

  private:
    // Whether an unmatched path may be answered from spa_fallback.
    bool spa_eligible(std::string_view normalized) const;

    StaticFilesConfig m_config;
    StaticTable m_table;
};

inline bool StaticFiles::spa_eligible(std::string_view normalized) const {
    if (m_config.spa_fallback.empty()) return false;
    if (!m_config.spa_fallback_extensionless_only) return true;
    const auto slash = normalized.rfind('/');
    const std::string_view last = slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
    return last.find('.') == std::string_view::npos;
}

inline asio::awaitable<bool> StaticFiles::try_serve(const RequestPtr& req, const ResponsePtr& res) const {
    PathError err{};
    auto normalized = decode_and_normalize(req->path(), err);
    if (!normalized) {
        // A malformed or traversal path never reaches the filesystem, and never
        // reaches the router either: it is answered here, with the connection
        // closed. A request target this shape says the peer is not a browser,
        // and there is nothing to gain by continuing to talk to it.
        SIMPLE_HTTP_WARN_LOG("static: rejecting path [{}] from {} (err={})", std::string{req->path()},
                             req->peer_address(), static_cast<int>(err));
        res->status(status::bad_request);
        detail::apply_common_headers(m_config.server_header, res, "text/plain; charset=utf-8", "no-store",
                                     std::nullopt, 0);
        (void)co_await res->send("400 Bad Request");
        (void)co_await res->close();
        co_return true;
    }

    // A decoded form that looks reserved steps aside rather than serving, even
    // when a file exists at that path.
    if (reserved(*normalized)) co_return false;

    const StaticEntry* entry = m_table.find(*normalized);
    bool spa_hit = false;
    if (entry == nullptr && spa_eligible(*normalized)) {
        entry = m_table.find("/" + m_config.spa_fallback);
        spa_hit = entry != nullptr;
    }
    if (entry == nullptr) co_return false;  // not ours: the caller renders 404

    const auto method = req->method();
    if (method != Method::Get && method != Method::Head) {
        // Resolved first, then rejected: a POST to a path that does not exist is
        // a 404, not a 405, which is what a static file server does. Without this
        // ordering a POST to "/" would be answered with the whole index page.
        res->status(status::method_not_allowed);
        if (!m_config.server_header.empty()) res->header(field::server, m_config.server_header);
        res->header(field::date, http_date(now_unix()));
        res->header(field::allow, "GET, HEAD");
        res->header(field::cache_control, "no-store");
        res->content_type("text/plain; charset=utf-8");
        (void)co_await res->send("405 Method Not Allowed");
        // Closing keeps the engine from draining a request body that was never
        // going to be used.
        (void)co_await res->close();
        co_return true;
    }

    const auto accept_encoding = parse_accept_encoding(req->header(field::accept_encoding).value_or(""));

    // A Range request is answered from the identity representation. Ranges over a
    // content-encoded body are well-defined, but the size in Content-Range would
    // then describe compressed bytes, which is a permanent source of confusion
    // for no gain: browsers do not range-request scripts or stylesheets. Falling
    // back to identity keeps the whole path trivially correct.
    const bool is_get = method == Method::Get;
    const bool is_head = method == Method::Head;
    const bool wants_range = is_get && req->header(field::range).has_value();

    const StaticRepresentation* rep = nullptr;
    if (wants_range) {
        rep = &entry->identity;
    } else {
        rep = detail::choose_representation(*entry, accept_encoding);
        if (rep == nullptr) {
            // Every encoding present is refused, identity included.
            res->status(status::not_acceptable);
            detail::apply_common_headers(m_config.server_header, res, "text/plain; charset=utf-8", "no-store",
                                         std::nullopt, 0);
            (void)co_await res->send("406 Not Acceptable");
            co_return true;
        }
    }

    // The SPA fallback answers with the fallback page's own bytes under the URL
    // that missed, so its validators must not come along: a client that cached
    // this URL with index.html's ETag would revalidate later, be told 304 by a
    // URL that is still the fallback, and never see the file once it exists.
    // Omitting them leaves nothing to revalidate against, so the page is fetched
    // every time — which is what `no-cache` means anyway.
    const std::string cache_control = spa_hit ? "no-cache" : m_table.cache_control_for(*entry);
    const std::optional<std::string> etag = spa_hit ? std::nullopt : std::optional<std::string>{rep->etag};
    const std::int64_t mtime = spa_hit ? 0 : entry->mtime;

    const std::string_view encoding =
        wants_range ? std::string_view{} : detail::content_encoding_of(*entry, *rep);

    auto apply_headers = [&] {
        detail::apply_common_headers(m_config.server_header, res, entry->content_type, cache_control, etag, mtime);
        if (!encoding.empty()) res->header(field::content_encoding, std::string{encoding});
        res->header(field::accept_ranges, "bytes");
    };

    // --- conditional requests (RFC 9110 §13.2.2 order) ---
    if (!spa_hit) {
        if (auto inm = req->header(field::if_none_match)) {
            if (etag_matches(*inm, rep->etag)) {
                res->status(status::not_modified);
                apply_headers();
                // No content-length on a 304 (RFC 9110 §15.4.5), and no body: the
                // response ends at the header block.
                (void)co_await res->send_bodyless();
                co_return true;
            }
        } else if (auto ims = req->header(field::if_modified_since)) {
            if (auto t = parse_http_date(*ims); t && entry->mtime <= *t) {
                res->status(status::not_modified);
                apply_headers();
                (void)co_await res->send_bodyless();
                co_return true;
            }
        }
    }

    // --- range ---
    std::optional<ByteRange> range;
    if (wants_range) {
        bool unsatisfiable = false;
        range = parse_range(*req->header(field::range), rep->size, unsatisfiable);
        if (range) {
            // If-Range: only honour the range when the client's validator still
            // describes this representation; otherwise send the whole thing.
            if (auto ir = req->header(field::if_range)) {
                const bool matches = ir->starts_with("\"") || ir->starts_with("W/")
                                         ? etag_matches(*ir, rep->etag)
                                         : parse_http_date(*ir).value_or(-1) == entry->mtime;
                if (!matches) range.reset();
            }
        } else if (unsatisfiable) {
            res->status(status::range_not_satisfiable);
            apply_headers();
            res->header(field::content_range, "bytes */" + std::to_string(rep->size));
            // send() with an empty body, NOT send_bodyless(): only 204/304 and
            // responses to HEAD are body-less *by rule*, so only those may omit
            // the length. A 416 that omits content-length and has no
            // transfer-encoding is delimited by connection close in HTTP/1.1 --
            // the client waits for the idle watchdog to fire instead of reading
            // a response that is already complete.
            res->header(field::content_length, "0");
            (void)co_await res->send("");
            co_return true;
        }
        // Otherwise parse_range declined (multi-range or a malformed spec): fall
        // through and send the full representation, which RFC 9110 permits.
    }

    auto body = m_table.read_body(*rep);
    if (!body) {
        // The file disappeared between startup and now. Treat it as a miss so the
        // normal 404 path answers, rather than emitting an empty 200.
        SIMPLE_HTTP_WARN_LOG("static: {} vanished after startup scan", rep->disk_path);
        co_return false;
    }

    res->status(range ? status::partial_content : status::ok);
    apply_headers();
    if (range) {
        res->header(field::content_range,
                    std::format("bytes {}-{}/{}", range->first, range->last, rep->size));
    }
    if (spa_hit) {
        // The fallback page is answered under a URL that may later name a real
        // file; revalidating it is the only way to notice. Its own ETag is
        // deliberately absent (see above), so the length is the only thing to
        // state.
        res->header(field::content_length, std::to_string(rep->size));
    } else if (rep->size == 0) {
        // An empty file still needs an explicit length: HTTP/2 forwards our
        // headers verbatim and never computes one, and a response without
        // content-length is framed by END_STREAM there and by chunked transfer
        // coding in HTTP/1.1 -- two different shapes for one file.
        res->header(field::content_length, "0");
    } else if (is_head) {
        // HEAD carries the headers a GET would produce, and no body. Its length
        // must be set by hand: the engine writes headers only, and the one-shot
        // send() would compute the length of the empty body instead.
        res->header(field::content_length, std::to_string(range ? range->length() : rep->size));
    } else if (range) {
        res->header(field::content_length, std::to_string(range->length()));
    } else if (!encoding.empty()) {
        // HTTP/1.1 recomputes content-length from the body we hand it; HTTP/2
        // forwards the header block untouched. Setting it makes both protocols
        // emit the same bytes.
        res->header(field::content_length, std::to_string(rep->size));
    }

    if (is_head) {
        (void)co_await res->send_bodyless();
        co_return true;
    }

    if (range) {
        (void)co_await res->send(
            body->substr(static_cast<std::size_t>(range->first), static_cast<std::size_t>(range->length())));
    } else {
        (void)co_await res->send(std::move(*body));
    }
    co_return true;
}

inline asio::awaitable<void> StaticFiles::serve_not_found(const RequestPtr& req, const ResponsePtr& res) const {
    res->status(status::not_found);
    if (!m_config.server_header.empty()) res->header(field::server, m_config.server_header);
    res->header(field::date, http_date(now_unix()));
    res->header(field::cache_control, "no-cache");

    const StaticEntry* page = m_table.not_found_entry();
    std::string payload;
    bool have_page = false;
    // The 404 page is preloaded by load() regardless of the size cap, so this is
    // a copy rather than a blocking read on an io_context thread.
    if (page != nullptr && page->identity.body) {
        payload = *page->identity.body;
        have_page = true;
    }

    // content_type() appends, so it must be called exactly once per response:
    // setting it here and again in a branch would emit two.
    res->content_type(have_page ? page->content_type : std::string{"text/plain; charset=utf-8"});
    if (!have_page) payload = "404 Not Found";

    if (req->method() == Method::Head) {
        res->header(field::content_length, std::to_string(payload.size()));
        (void)co_await res->send_bodyless();
        co_return;
    }
    (void)co_await res->send(std::move(payload));
    co_return;
}

}  // namespace simple_http
