#pragma once

// A document root turned into an immutable lookup table.
//
// The shape of this module is decided by one security property: **the request
// path is never used to touch the filesystem**. At startup the whole root is
// walked once and turned into a table of "canonical path -> file metadata"; at
// request time a URL is decoded, normalized (core/url_path.h) and looked up in
// that table. Nothing else. That single decision removes, by construction:
//
//   * directory traversal -- ".." cannot be expressed in a normalized key, and
//     even if it could, no such key exists;
//   * symlink escape -- symlinks are skipped during the scan and never followed;
//   * TOCTOU -- no stat()/open() happens on a request-chosen path;
//   * FIFO/device hangs -- only regular files are ever admitted to the table.
//
// It also makes the cache validators free: ETag and Last-Modified come from the
// scan's stat() results, so a 304 is answered without opening the file.
//
// The alternatives are worse in ways that are easy to miss. Sanitizing and then
// stat()ing still stats a path the peer chose, and leaves a window between check
// and use. Joining and then testing containment is what the `%2e%2e%2f` family
// is built to defeat; url_path.h refuses an escaped separator instead. Following
// symlinks and then checking where they landed reopens the escape if the link is
// replaced afterwards.
//
// This header is protocol-agnostic on purpose: no Request, no Response, no
// Asio. Selecting a representation and answering a request live in
// handler/static_files.h, which is the half that needs them.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "accept_encoding.h"  // AcceptEncoding, coding_q, identity_q
#include "logging.h"
#include "mime.h"      // by_extension
#include "url_path.h"  // under_prefix
#include "validators.h"

namespace simple_http {

// What a document root is scanned with. Every field is a deployment choice; the
// defaults are conservative rather than convenient (see the two that are empty).
struct StaticTableConfig {
    // The document root. Empty disables the whole component: the serving stage
    // steps aside and every request falls through to whatever else is routed,
    // which is the switch that turns a server back into a plain proxy.
    std::string root;
    // Answers a directory URL too ("/" from index.html, "/blog" from
    // blog/index.html), the way nginx's `index` and Apache's DirectoryIndex do.
    std::string index_file{"index.html"};
    // The site's own 404 page, served relative to the root. Also what an
    // extensionless lookup ("/blog/foo") falls back to when no ".html" sibling
    // exists — with no such file, a plain-text body is used instead.
    std::string not_found_file{"404.html"};
    // Paths under these prefixes are marked immutable, so they are answered with
    // `Cache-Control: public, max-age=31536000, immutable`. They are meant for
    // build output whose file names carry a content hash.
    //
    // Empty by default, and that is a safety decision rather than an oversight:
    // marking a URL immutable when its contents can change is a correctness bug
    // (it promises a year of cache for bytes that will differ), while failing to
    // mark one is only a performance loss. The safe direction is off. Same
    // reasoning as CompressionConfig::enabled.
    std::vector<std::string> immutable_prefixes{};
    // Files that must not live inside the root. If any of them resolves to a
    // path under it, load() refuses.
    //
    // The case this exists for is `root: "."`, which would publish the server's
    // own configuration file — one character away from a reasonable-looking
    // value, and silently fatal. The check runs *before* the walk, so a
    // misconfigured root fails immediately instead of first scanning (and
    // preloading) an entire source tree and rejecting it afterwards.
    std::vector<std::string> must_not_contain{};
    // Files at or below this size are read into memory at startup, so serving
    // one never touches the filesystem: no blocking read on an io_context
    // thread, and no window in which the file on disk could differ from what was
    // scanned. Larger files are read on demand.
    std::uint64_t preload_max_file_bytes{1u << 20};
    // Total preload budget. Once it is spent, later files are served from disk.
    std::uint64_t preload_budget_bytes{64u << 20};
};

// One encoding of one file: the identity bytes, or its .br / .gz sibling.
struct StaticRepresentation {
    std::string disk_path;
    std::uint64_t size{0};
    std::string etag;
    // Non-null when the file was small enough to be read at startup, in which
    // case serving it involves no filesystem access at all. Shared so the entry
    // stays immutable and a coroutine can keep the bytes alive cheaply.
    std::shared_ptr<const std::string> body;
};

struct StaticEntry {
    std::string content_type;
    std::int64_t mtime{0};
    bool immutable{false};  // under a content-hashed prefix: cache forever

    StaticRepresentation identity;
    std::optional<StaticRepresentation> br;
    std::optional<StaticRepresentation> gz;
};

namespace detail {

// Whether `candidate` lies inside (or equals) `root`. Both must already be
// canonical — the caller's job, via std::filesystem::canonical.
//
// Compares path *components*, never a string prefix: "/srv/web/dist-evil"
// starts with the characters of "/srv/web/dist" and would pass a starts_with
// check while being a completely different directory.
inline bool path_is_under(const std::string& root, const std::string& candidate) {
    const std::filesystem::path r{root};
    const std::filesystem::path c{candidate};
    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci) {
        if (ci == c.end() || *ri != *ci) return false;
    }
    return true;
}

// Converts a file timestamp to Unix seconds.
//
// std::filesystem::file_time_type's epoch is *unspecified* — it is not the Unix
// epoch, so casting its duration straight to seconds produces a meaningless
// number (observed: a plausible-looking but wildly wrong year, which then also
// poisons the ETag). clock_cast is the portable conversion between clocks.
inline std::int64_t file_time_to_unix(const std::filesystem::file_time_type& t) {
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}

// Picks the best acceptable encoding. Ties go to brotli (it is checked first and
// the comparison is strict), and identity only wins when it is strictly
// preferred — so a plain `Accept-Encoding: gzip, deflate, br` still gets brotli.
//
// Returns nullptr when nothing at all is acceptable, which is a 406.
inline const StaticRepresentation* choose_representation(const StaticEntry& entry, const AcceptEncoding& ae) {
    const double q_br = coding_q(ae.br, ae);
    const double q_gz = coding_q(ae.gzip, ae);
    const double q_id = identity_q(ae);

    const StaticRepresentation* best = nullptr;
    double best_q = 0.0;
    if (entry.br && q_br > best_q) {
        best = &*entry.br;
        best_q = q_br;
    }
    if (entry.gz && q_gz > best_q) {
        best = &*entry.gz;
        best_q = q_gz;
    }
    if (q_id > best_q) {
        best = &entry.identity;
        best_q = q_id;
    }
    return best;
}

inline std::string_view content_encoding_of(const StaticEntry& entry, const StaticRepresentation& rep) {
    if (entry.br && &*entry.br == &rep) return "br";
    if (entry.gz && &*entry.gz == &rep) return "gzip";
    return {};
}

// Hash and equality that accept string_view, so a lookup never has to build a
// std::string key. Same technique as handler/router.h.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

}  // namespace detail

class StaticTable {
  public:
    struct Stats {
        std::size_t files{0};
        std::size_t keys{0};
        std::size_t preloaded{0};
        std::uint64_t preloaded_bytes{0};
        std::size_t brotli{0};
        std::size_t gzip{0};
        std::size_t skipped_symlinks{0};
        std::size_t skipped_other{0};
    };

    explicit StaticTable(StaticTableConfig config) : m_config(std::move(config)) {}

    // Walks the root and builds the table. Returns false, with `error` set, when
    // the root is unusable or must_not_contain is violated.
    //
    // Must run to completion before the first request is served: the second pass
    // attaches pre-compressed siblings to entries the first pass created, and
    // that relies on no reader existing yet. This is not a hot-reload API — the
    // table is built once and read for the life of the process.
    bool load(std::string& error);

    bool enabled() const noexcept { return !m_root.empty(); }
    const Stats& stats() const noexcept { return m_stats; }
    const std::string& root() const noexcept { return m_root; }  // canonical, for logging only

    // Read-only view. The table is built entirely inside load() and never
    // mutated afterwards, so handing out const pointers is sound and lets every
    // worker thread look up concurrently without a lock.
    const StaticEntry* find(std::string_view key) const {
        auto it = m_table.find(key);
        return it == m_table.end() ? nullptr : it->second.get();
    }

    // The site's 404 page, or nullptr when none was built.
    const StaticEntry* not_found_entry() const noexcept { return m_not_found.get(); }

    std::string cache_control_for(const StaticEntry& entry) const;

    // A representation's bytes, from the preloaded copy when there is one and
    // from disk otherwise. nullopt means the file has gone missing since the
    // scan — a miss, not an empty body. Public because the serving half lives in
    // another layer and needs exactly this.
    std::optional<std::string> read_body(const StaticRepresentation& rep) const;

  private:
    // Non-const so load() can attach pre-compressed siblings; every read path
    // goes through the const overloads, so nothing outside load() can mutate.
    using EntryPtr = std::shared_ptr<StaticEntry>;

    StaticTableConfig m_config;
    std::string m_root;  // canonical, for logging only

    std::uint64_t m_preload_max{0};
    std::uint64_t m_preload_budget{0};

    // Built once by load(), then read-only for the life of the process. Every
    // worker thread reads it concurrently; nothing mutates it, so no lock is
    // needed — and none may be added without also revisiting the serving half,
    // which deliberately holds no lock across any co_await.
    std::unordered_map<std::string, EntryPtr, detail::StringHash, std::equal_to<>> m_table;

    EntryPtr m_not_found;

    Stats m_stats{};
};

inline bool StaticTable::load(std::string& error) {
    m_root.clear();
    m_table.clear();
    m_not_found.reset();
    m_stats = Stats{};
    m_preload_max = m_config.preload_max_file_bytes;
    m_preload_budget = m_config.preload_budget_bytes;

    if (m_config.root.empty()) return true;  // disabled

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::canonical(m_config.root, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) {
        error = "static root is not a readable directory: " + m_config.root;
        return false;
    }
    m_root = root.string();

    // Before anything expensive: a file that must never be downloadable cannot
    // be allowed to sit inside the document root. Running this ahead of the walk
    // is what keeps a `root: "."` mistake from first reading the whole tree.
    for (const auto& protected_path : m_config.must_not_contain) {
        if (protected_path.empty()) continue;
        std::error_code pec;
        const auto resolved = std::filesystem::canonical(protected_path, pec);
        if (pec) continue;  // cannot resolve it: not something we can be serving
        if (detail::path_is_under(m_root, resolved.string())) {
            error = "static root " + m_root + " contains " + resolved.string() + ", which must never be served";
            m_root.clear();
            return false;
        }
    }

    // Collect every regular file, skipping symlinks outright rather than
    // following them: a symlink inside the document root is the one way a file
    // outside it could otherwise become reachable, and there is no way to tell a
    // benign link from a planted one at scan time.
    std::vector<std::string> rels;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = "cannot walk static root: " + ec.message();
        return false;
    }
    for (const auto& dir_entry : it) {
        std::error_code se;
        if (dir_entry.is_symlink(se)) {
            ++m_stats.skipped_symlinks;
            if (dir_entry.is_directory(se)) it.disable_recursion_pending();
            continue;
        }
        if (!dir_entry.is_regular_file(se)) {
            ++m_stats.skipped_other;  // directories, FIFOs, sockets, devices
            continue;
        }
        std::error_code re;
        auto rel = std::filesystem::relative(dir_entry.path(), root, re);
        if (re) continue;
        rels.push_back(rel.generic_string());
    }
    std::sort(rels.begin(), rels.end());  // deterministic keys and preload order

    auto is_precompressed = [](std::string_view rel) { return rel.ends_with(".br") || rel.ends_with(".gz"); };

    auto make_representation = [&](const std::string& rel, std::uint64_t& budget) -> StaticRepresentation {
        StaticRepresentation rep;
        rep.disk_path = (root / rel).string();
        std::error_code se;
        rep.size = static_cast<std::uint64_t>(std::filesystem::file_size(rep.disk_path, se));
        if (se) rep.size = 0;
        const auto mtime = std::filesystem::last_write_time(rep.disk_path, se);
        const std::int64_t mtime_s = se ? 0 : detail::file_time_to_unix(mtime);

        std::string suffix;
        if (rel.ends_with(".br")) {
            suffix = "br";
        } else if (rel.ends_with(".gz")) {
            suffix = "gz";
        }
        rep.etag = make_etag(mtime_s, rep.size, suffix);

        if (rep.size <= m_preload_max && rep.size <= budget) {
            std::ifstream in(rep.disk_path, std::ios::binary);
            if (in) {
                auto buf = std::make_shared<std::string>();
                buf->resize(static_cast<std::size_t>(rep.size));
                in.read(buf->data(), static_cast<std::streamsize>(rep.size));
                if (in.gcount() == static_cast<std::streamsize>(rep.size)) {
                    rep.body = std::move(buf);
                    budget -= rep.size;
                    ++m_stats.preloaded;
                    m_stats.preloaded_bytes += rep.size;
                }
            }
        }
        return rep;
    };

    std::uint64_t budget = m_preload_budget;

    // Pass 1: the base files.
    for (const auto& rel : rels) {
        if (is_precompressed(rel)) continue;

        auto entry = std::make_shared<StaticEntry>();
        entry->identity = make_representation(rel, budget);
        entry->content_type = std::string{mime::by_extension(rel)};

        std::error_code se;
        const auto mtime = std::filesystem::last_write_time(entry->identity.disk_path, se);
        entry->mtime = se ? 0 : detail::file_time_to_unix(mtime);

        const std::string url = "/" + rel;
        for (const auto& p : m_config.immutable_prefixes) {
            if (under_prefix(url, p)) {
                entry->immutable = true;
                break;
            }
        }

        EntryPtr shared = entry;
        auto insert_keys = [&](const std::string& key) {
            auto [pos, inserted] = m_table.emplace(key, shared);
            if (!inserted) {
                SIMPLE_HTTP_WARN_LOG("static: key collision on {} ({} vs {})", key,
                                     pos->second->identity.disk_path, entry->identity.disk_path);
            } else {
                ++m_stats.keys;
            }
        };

        insert_keys(url);

        // index.html also answers its directory: "" for the root, "blog" for
        // blog/index.html.
        if (rel.size() >= m_config.index_file.size() &&
            rel.compare(rel.size() - m_config.index_file.size(), m_config.index_file.size(), m_config.index_file) ==
                0) {
            std::string dir = rel.substr(0, rel.size() - m_config.index_file.size());
            while (!dir.empty() && dir.back() == '/') dir.pop_back();
            insert_keys(dir.empty() ? "/" : "/" + dir);
        } else if (rel.ends_with(".html")) {
            // No 301 from /blog/foo to /blog/foo.html: a redirect is an
            // open-redirect surface for no benefit, and answering both is what a
            // `try_files $uri $uri.html $uri/index.html` config does anyway.
            insert_keys(url.substr(0, url.size() - 5));
        }

        if (rel == m_config.not_found_file) {
            m_not_found = shared;
        }
        ++m_stats.files;
    }

    // Pass 2: attach pre-compressed siblings. After pass 1 because the walk
    // order is unspecified and "a.js.br" may come before "a.js".
    for (const auto& rel : rels) {
        std::string base;
        bool is_br = false;
        if (rel.ends_with(".br")) {
            base = rel.substr(0, rel.size() - 3);
            is_br = true;
        } else if (rel.ends_with(".gz")) {
            base = rel.substr(0, rel.size() - 3);
        } else {
            continue;
        }
        auto it2 = m_table.find("/" + base);
        if (it2 == m_table.end()) {
            SIMPLE_HTTP_WARN_LOG("static: {} has no base file, ignoring", rel);
            continue;
        }
        auto rep = make_representation(rel, budget);
        // Safe to mutate: load() runs to completion before the server accepts a
        // single connection, and every read path only ever sees a const entry.
        if (is_br) {
            it2->second->br = std::move(rep);
            ++m_stats.brotli;
        } else {
            it2->second->gz = std::move(rep);
            ++m_stats.gzip;
        }
    }

    // The 404 page is loaded regardless of the size cap. It is one file, and the
    // alternative — reading it synchronously on a miss — is exactly the blocking
    // read on an io_context thread that preloading exists to avoid.
    if (m_not_found && !m_not_found->identity.body) {
        if (auto body = read_body(m_not_found->identity)) {
            m_not_found->identity.body = std::make_shared<const std::string>(std::move(*body));
        }
    }

    SIMPLE_HTTP_INFO_LOG("static table loaded: root={} files={} keys={} preloaded={} ({} KiB) br={} gz={} "
                         "skipped(symlink={} other={})",
                         m_root, m_stats.files, m_stats.keys, m_stats.preloaded, m_stats.preloaded_bytes / 1024,
                         m_stats.brotli, m_stats.gzip, m_stats.skipped_symlinks, m_stats.skipped_other);
    return true;
}

inline std::string StaticTable::cache_control_for(const StaticEntry& entry) const {
    if (entry.immutable) {
        // Content-hashed: the bytes behind this URL can never change.
        return "public, max-age=31536000, immutable";
    }
    if (entry.content_type.starts_with("text/html")) {
        // Revalidate every time. This is also what keeps conditional requests
        // exercised in a normal browsing session: a cache that never revalidates
        // hides the whole ETag path behind a browser's private cache.
        return "no-cache";
    }
    if (entry.content_type.starts_with("application/json") || entry.content_type.starts_with("application/xml") ||
        entry.content_type.starts_with("application/manifest")) {
        return "public, max-age=3600";
    }
    return "public, max-age=86400";
}

inline std::optional<std::string> StaticTable::read_body(const StaticRepresentation& rep) const {
    if (rep.body) return *rep.body;  // preloaded: copy only
    std::ifstream in(rep.disk_path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string out;
    out.resize(static_cast<std::size_t>(rep.size));
    in.read(out.data(), static_cast<std::streamsize>(rep.size));
    if (in.gcount() != static_cast<std::streamsize>(rep.size)) return std::nullopt;
    return out;
}

}  // namespace simple_http
