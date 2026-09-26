// handler/static_files.h + core/static_table.h: the static file stage.
//
// The tests split in two. The first half drives the table directly and is mostly
// about the security property — "the request path never touches the filesystem"
// is what makes traversal, symlink escape and TOCTOU structural rather than
// checked, so it is tested by trying to break it rather than by asserting the
// checks exist. The second half drives real requests through FakeResponseWriter
// and is about the wire, where several header values are set by hand for reasons
// that are easy to lose.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include "simple_http.h"
#include "../static_fixture.h"
#include "test_support.h"

using namespace simple_http;
using namespace simple_http::test;

namespace {

RequestPtr make_request(asio::io_context& ctx, std::string path, Method method = Method::Get) {
    auto req = std::make_shared<Request>(Version::Http11, ctx.get_executor(), asio::ip::tcp::endpoint{});
    req->set_target(std::move(path));
    req->set_method(method);
    // The engines always end the request body; a handler that reads it would
    // otherwise wait forever.
    req->body().finish();
    return req;
}

void add_header(const RequestPtr& req, std::string name, std::string value) {
    req->mutable_headers().add(std::move(name), std::move(value));
}

// A site over the fixture's standard tree, already loaded.
StaticFilesConfig standard_config(const StaticFixture& fx) {
    StaticFilesConfig cfg;
    cfg.table.root = fx.root_string();
    return cfg;
}

std::shared_ptr<StaticFiles> load_standard(const StaticFixture& fx, StaticFilesConfig cfg = {}) {
    if (cfg.table.root.empty()) cfg.table.root = fx.root_string();
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE(site->load(error));
    CHECK(error.empty());
    return site;
}

// The result of one request: the recorded writer, and whether the site claimed it.
struct Served {
    std::shared_ptr<FakeResponseWriter> writer;
    bool handled = false;
};

Served serve(const StaticFiles& site, asio::io_context& ctx, const RequestPtr& req) {
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    auto handled = run_on(ctx, site.try_serve(req, res));
    REQUIRE(handled.has_value());
    return Served{writer, *handled};
}

Served get(const StaticFiles& site, asio::io_context& ctx, std::string path) {
    auto req = make_request(ctx, std::move(path));
    return serve(site, ctx, req);
}

}  // namespace

// ---------------------------------------------------------------------------
// The table: what got in, and what cannot
// ---------------------------------------------------------------------------

TEST_CASE("static: index files answer their directory, and .html answers both", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    CHECK(site->find("/") != nullptr);
    CHECK(site->find("/index.html") != nullptr);
    // Both keys are the same file, not two copies of it.
    CHECK(site->find("/") == site->find("/index.html"));

    CHECK(site->find("/blog") != nullptr);
    CHECK(site->find("/blog") == site->find("/blog/index.html"));

    // "/blog/post" answers the same file as "/blog/post.html" — no redirect,
    // which would be an open-redirect surface for no benefit.
    CHECK(site->find("/blog/post") != nullptr);
    CHECK(site->find("/blog/post") == site->find("/blog/post.html"));

    CHECK(site->find("/app.js") != nullptr);
    CHECK(site->find("/nonexistent") == nullptr);
}

TEST_CASE("static: no hostile input can produce a key", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    // The property is about the whole space, not about a list of strings: for
    // every adversary-shaped input, either decoding refuses it or the resulting
    // key is simply absent. There is no third outcome, which is what "structural
    // rather than checked" means.
    const char* hostile[] = {
        "/../../etc/passwd",
        "/a/../../b",
        "/%2e%2e%2fetc%2fpasswd",
        "/..%5c..%5c",
        "/....//",
        "/.%2e/",
        "/%2e%2e",
        "/a/%2e%2e/%2e%2e/etc/passwd",
    };
    for (const char* raw : hostile) {
        INFO("raw: " << raw);
        PathError err{};
        auto key = decode_and_normalize(raw, err);
        if (key) {
            CHECK(site->find(*key) == nullptr);
        } else {
            CHECK(err != PathError::None);
        }
    }

    // And the table itself carries no key that could ever be a traversal.
    for (const char* base : {"/", "/blog", "/assets"}) {
        for (const char* leaf : {"a", "b", "index.html", "app.js"}) {
            const std::string key = std::string{base} + (std::string{base} == "/" ? "" : "/") + leaf;
            CHECK(key.find("..") == std::string::npos);
            CHECK(key.find('\\') == std::string::npos);
        }
    }
}

#ifndef _WIN32
TEST_CASE("static: symlinks are skipped, never followed", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    // A symlink to a file outside the root, and one to a directory outside it.
    CHECK(site->find("/escape") == nullptr);
    CHECK(site->find("/deep") == nullptr);

    // Nothing from the linked directory made it in either — the scan does not
    // descend through a symlinked directory.
    CHECK(site->stats().skipped_symlinks == 2);

    // The real files are still there: refusing symlinks must not weaken the
    // ordinary path.
    CHECK(site->find("/app.js") != nullptr);
}

TEST_CASE("static: a FIFO is never opened", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    // If the scanner opened this, the call would hang here rather than fail —
    // which is the whole point of admitting only regular files.
    auto site = load_standard(fx);

    CHECK(site->find("/pipe") == nullptr);
    CHECK(site->stats().skipped_other >= 1);
}
#else
TEST_CASE("static: symlink and FIFO cases are POSIX-only", "[static]") { SUCCEED(); }
#endif

TEST_CASE("static: a file that vanishes after the scan is a miss, not an empty 200", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    // Low enough that big.bin is not preloaded: the request has to go to disk,
    // which is what makes it observable.
    cfg.table.preload_max_file_bytes = 64;
    auto site = load_standard(fx, cfg);

    CHECK(site->find("/big.bin") != nullptr);

    // Move the whole root away. Nothing the request does touches the filesystem
    // until read_body(), so the preloaded files must still serve...
    const auto moved = fx.root_string() + ".moved";
    std::filesystem::rename(fx.root(), moved);

    asio::io_context ctx;

    auto preloaded = get(*site, ctx, "/index.html");
    CHECK(preloaded.handled);
    CHECK(preloaded.writer->last_status == 200);

    // ...and the one that has to read must report a miss, so the caller's 404
    // path answers instead of the server inventing an empty 200.
    auto gone = get(*site, ctx, "/big.bin");
    CHECK_FALSE(gone.handled);
    CHECK(gone.writer->last_status == 0);  // nothing written at all

    std::filesystem::rename(moved, fx.root());  // so the fixture's cleanup is a no-op, not an error
}

TEST_CASE("static: must_not_contain refuses a root that would publish it", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    fx.write("secret.json", R"({"uuid":"do-not-serve-me"})");

    {
        // Inside the root: refused outright.
        StaticFilesConfig cfg = standard_config(fx);
        cfg.table.must_not_contain = {(fx.root() / "secret.json").string()};
        auto site = std::make_shared<StaticFiles>(std::move(cfg));
        std::string error;
        CHECK_FALSE(site->load(error));
        CHECK_FALSE(error.empty());
        CHECK_FALSE(site->enabled());
    }
    {
        // Outside the root: nothing to object to.
        StaticFilesConfig cfg = standard_config(fx);
        cfg.table.must_not_contain = {(fx.root().parent_path() / "elsewhere.json").string()};
        auto site = std::make_shared<StaticFiles>(std::move(cfg));
        std::string error;
        CHECK(site->load(error));
    }
}

TEST_CASE("static: must_not_contain is checked before the tree is walked", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    fx.write("secret.json", "x");

    StaticFilesConfig cfg = standard_config(fx);
    cfg.table.must_not_contain = {(fx.root() / "secret.json").string()};
    auto site = std::make_shared<StaticFiles>(std::move(cfg));
    std::string error;
    REQUIRE_FALSE(site->load(error));

    // The check runs ahead of the walk, so a misconfigured root costs nothing:
    // no files were collected, which is observable because the stats are reset
    // at the top of load() and the failure path returns before pass 1.
    CHECK(site->stats().files == 0);
    CHECK(site->stats().keys == 0);
}

TEST_CASE("static: reserved prefixes step aside even when a file exists", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.reserved_prefixes = {"/assets", "/blog/"};
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;

    // The file is in the table — defense in depth is about the serving stage,
    // not about pretending the file is not there.
    CHECK(site->find("/assets/chunk-abc123.js") != nullptr);

    auto blocked = get(*site, ctx, "/assets/chunk-abc123.js");
    CHECK_FALSE(blocked.handled);  // nothing written: the caller's 404 takes over
    CHECK(blocked.writer->last_status == 0);

    // Segment-aware: a path that merely shares the characters is not reserved.
    CHECK(site->reserved("/assets/chunk-abc123.js"));
    CHECK(site->reserved("/blog/post.html"));  // the prefix ends in '/'
    CHECK_FALSE(site->reserved("/assetsevil"));
    CHECK_FALSE(site->reserved("/blogx"));
}

TEST_CASE("static: cache policy follows the entry's kind", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.table.immutable_prefixes = {"/assets/"};
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;

    auto html = get(*site, ctx, "/index.html");
    REQUIRE(html.writer->has_header("cache-control"));
    CHECK(html.writer->header("cache-control") == "no-cache");

    auto hashed = get(*site, ctx, "/assets/chunk-abc123.js");
    REQUIRE(hashed.writer->has_header("cache-control"));
    CHECK(hashed.writer->header("cache-control") == "public, max-age=31536000, immutable");

    auto plain = get(*site, ctx, "/app.js");
    REQUIRE(plain.writer->has_header("cache-control"));
    CHECK(plain.writer->header("cache-control") == "public, max-age=86400");
}

TEST_CASE("static: preload respects both the per-file cap and the budget", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    {
        // Cap below everything but the smallest: big.bin stays on disk.
        StaticFilesConfig cfg = standard_config(fx);
        cfg.table.preload_max_file_bytes = 32;
        auto site = load_standard(fx, cfg);

        const auto& stats = site->stats();
        CHECK(stats.preloaded > 0);
        CHECK(stats.preloaded_bytes <= cfg.table.preload_budget_bytes);

        // big.bin's identity representation is not in memory...
        const StaticEntry* big = site->find("/big.bin");
        REQUIRE(big != nullptr);
        CHECK(big->identity.body == nullptr);

        // ...while a small one is.
        const StaticEntry* small = site->find("/blog/post.html");
        REQUIRE(small != nullptr);
        CHECK(small->identity.body != nullptr);
    }
    {
        // Zero cap: nothing but the 404 page is preloaded. load() reads that one
        // regardless of the cap, because a miss must not do a blocking read on an
        // io_context thread — which is the whole reason preloading exists.
        StaticFilesConfig cfg = standard_config(fx);
        cfg.table.preload_max_file_bytes = 0;
        auto site = load_standard(fx, cfg);
        CHECK(site->stats().preloaded == 1);
        const StaticEntry* too_big = site->find("/big.bin");
        REQUIRE(too_big != nullptr);
        CHECK(too_big->identity.body == nullptr);

        asio::io_context ctx;
        auto r = get(*site, ctx, "/app.js");
        CHECK(r.handled);
        CHECK(r.writer->last_status == 200);
        CHECK(r.writer->last_body == "console.log('app');");
    }
}

TEST_CASE("static: an empty root disables the site rather than failing", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg;
    cfg.table.root = "";  // the switch back to a plain proxy
    auto site = std::make_shared<StaticFiles>(std::move(cfg));

    std::string error;
    CHECK(site->load(error));  // not an error: disabled
    CHECK(error.empty());
    CHECK_FALSE(site->enabled());

    asio::io_context ctx;
    auto r = get(*site, ctx, "/index.html");
    CHECK_FALSE(r.handled);
}

TEST_CASE("static: an unusable root fails, and says why", "[static]") {
    StaticFixture fx;

    {
        StaticFilesConfig cfg;
        cfg.table.root = (fx.root() / "does-not-exist").string();
        auto site = std::make_shared<StaticFiles>(std::move(cfg));
        std::string error;
        CHECK_FALSE(site->load(error));
        CHECK(error.find("root") != std::string::npos);
    }
    {
        // A regular file where a directory is expected.
        fx.write("afile", "x");
        StaticFilesConfig cfg;
        cfg.table.root = (fx.root() / "afile").string();
        auto site = std::make_shared<StaticFiles>(std::move(cfg));
        std::string error;
        CHECK_FALSE(site->load(error));
    }
}

// ---------------------------------------------------------------------------
// Serving: the bytes on the wire
// ---------------------------------------------------------------------------

TEST_CASE("static: a GET carries the headers a cache needs", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto r = get(*site, ctx, "/app.js");

    CHECK(r.handled);
    CHECK(r.writer->last_status == 200);
    CHECK(r.writer->last_body == "console.log('app');");

    CHECK(r.writer->has_header("etag"));
    CHECK(r.writer->has_header("last-modified"));
    CHECK(r.writer->has_header("date"));
    CHECK(r.writer->has_header("vary"));
    CHECK(r.writer->header("accept-ranges") == "bytes");
    CHECK(r.writer->header("content-type") == "text/javascript; charset=utf-8");
}

TEST_CASE("static: HEAD carries a GET's length and no body", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto r = serve(*site, ctx, make_request(ctx, "/app.js", Method::Head));

    CHECK(r.handled);
    CHECK(r.writer->last_status == 200);
    CHECK(r.writer->sent_bodyless);
    CHECK(r.writer->last_body.empty());

    // The length must be set by hand: the engines write headers only, and the
    // one-shot send() would compute the length of the empty body instead.
    REQUIRE(r.writer->has_header("content-length"));
    CHECK(r.writer->header("content-length") == std::to_string(std::string{"console.log('app');"}.size()));
}

TEST_CASE("static: an empty file gets an explicit zero length", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto r = get(*site, ctx, "/empty.txt");

    CHECK(r.handled);
    CHECK(r.writer->last_status == 200);
    // Explicit, because HTTP/1.1 would otherwise frame it as chunked and HTTP/2
    // as END_STREAM — two different byte sequences for one file.
    REQUIRE(r.writer->has_header("content-length"));
    CHECK(r.writer->header("content-length") == "0");
}

TEST_CASE("static: a conditional hit is a 304 with no length", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    const StaticEntry* entry = site->find("/app.js");
    REQUIRE(entry != nullptr);
    const std::string etag = entry->identity.etag;

    {
        auto req = make_request(ctx, "/app.js");
        add_header(req, "if-none-match", etag);
        auto r = serve(*site, ctx, req);
        CHECK(r.handled);
        CHECK(r.writer->last_status == 304);
        CHECK(r.writer->sent_bodyless);
        // RFC 9110 §15.4.5: a 304 carries no content-length.
        CHECK_FALSE(r.writer->has_header("content-length"));
    }
    {
        // A non-matching validator is a normal 200.
        auto req = make_request(ctx, "/app.js");
        add_header(req, "if-none-match", "\"something-else\"");
        auto r = serve(*site, ctx, req);
        CHECK(r.writer->last_status == 200);
        CHECK_FALSE(r.writer->last_body.empty());
    }
    {
        // If-Modified-Since at or after the file's mtime: 304.
        auto req = make_request(ctx, "/app.js");
        add_header(req, "if-modified-since", http_date(entry->mtime + 10));
        auto r = serve(*site, ctx, req);
        CHECK(r.writer->last_status == 304);
    }
    {
        // If-None-Match wins over If-Modified-Since when both are present and the
        // etag does not match: the else-if is deliberate (RFC 9110 §13.2.2).
        auto req = make_request(ctx, "/app.js");
        add_header(req, "if-none-match", "\"nope\"");
        add_header(req, "if-modified-since", http_date(entry->mtime + 10));
        auto r = serve(*site, ctx, req);
        CHECK(r.writer->last_status == 200);
    }
}

TEST_CASE("static: a range is a 206 over the identity representation", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto req = make_request(ctx, "/app.js");
    // Range must ignore the negotiated encoding: Content-Range would otherwise
    // describe compressed bytes.
    add_header(req, "accept-encoding", "br");
    add_header(req, "range", "bytes=0-6");
    auto r = serve(*site, ctx, req);

    CHECK(r.handled);
    CHECK(r.writer->last_status == 206);
    CHECK(r.writer->last_body == "console");  // bytes 0-6 inclusive
    CHECK(r.writer->header("content-range") == "bytes 0-6/19");
    CHECK(r.writer->header("content-length") == "7");
    CHECK_FALSE(r.writer->has_header("content-encoding"));
}

TEST_CASE("static: an unsatisfiable range is a 416 that does not hang the client", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto req = make_request(ctx, "/app.js");
    add_header(req, "range", "bytes=999-");
    auto r = serve(*site, ctx, req);

    CHECK(r.handled);
    CHECK(r.writer->last_status == 416);
    CHECK(r.writer->header("content-range") == "bytes */19");

    // send() with an empty body, NOT send_bodyless(): only 204/304 and HEAD are
    // body-less by rule, so only those may omit the length. A 416 without one is
    // delimited by connection close in HTTP/1.1 and the client waits for the idle
    // watchdog.
    CHECK_FALSE(r.writer->sent_bodyless);
    REQUIRE(r.writer->has_header("content-length"));
    CHECK(r.writer->header("content-length") == "0");
}

TEST_CASE("static: a malformed path is answered here, with the connection closed", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    for (const char* raw : {"/%2e%2e/etc/passwd", "/a%2fb", "/a\\b"}) {
        INFO("raw: " << raw);
        auto r = get(*site, ctx, raw);
        CHECK(r.handled);  // answered, not passed on
        CHECK(r.writer->last_status == 400);
        CHECK(r.writer->header("cache-control") == "no-store");
        CHECK_FALSE(r.writer->open);
    }
}

TEST_CASE("static: a method other than GET/HEAD is resolved before it is rejected", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;

    {
        // A path that exists: 405, with an Allow.
        auto r = serve(*site, ctx, make_request(ctx, "/index.html", Method::Post));
        CHECK(r.handled);
        CHECK(r.writer->last_status == 405);
        CHECK(r.writer->header("allow") == "GET, HEAD");
        CHECK_FALSE(r.writer->open);
    }
    {
        // A path that does not: 404, not 405 — which is what a static file
        // server does, and what stops a POST to "/" from receiving the index.
        auto r = serve(*site, ctx, make_request(ctx, "/nope", Method::Post));
        CHECK_FALSE(r.handled);
        CHECK(r.writer->last_status == 0);
    }
}

TEST_CASE("static: pre-compressed siblings are negotiated, never re-compressed", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto fetch = [&](const char* accept) {
        auto req = make_request(ctx, "/app.js");
        if (accept != nullptr) add_header(req, "accept-encoding", accept);
        return serve(*site, ctx, req);
    };

    {
        auto r = fetch("gzip, deflate, br");
        CHECK(r.writer->last_status == 200);
        REQUIRE(r.writer->has_header("content-encoding"));
        CHECK(r.writer->header("content-encoding") == "br");
        CHECK(r.writer->last_body == "BROTLI:console.log('app');");
        // The length is set by hand so HTTP/1.1 and HTTP/2 emit the same bytes.
        CHECK(r.writer->header("content-length") ==
              std::to_string(std::string{"BROTLI:console.log('app');"}.size()));
    }
    {
        // br refused explicitly: gzip is still acceptable.
        auto r = fetch("gzip, br;q=0");
        REQUIRE(r.writer->has_header("content-encoding"));
        CHECK(r.writer->header("content-encoding") == "gzip");
    }
    {
        // Neither accepted: identity.
        auto r = fetch("identity");
        CHECK_FALSE(r.writer->has_header("content-encoding"));
        CHECK(r.writer->last_body == "console.log('app');");
    }
    {
        // Everything refused, identity included: 406.
        auto r = fetch("br;q=0, gzip;q=0, identity;q=0");
        CHECK(r.handled);
        CHECK(r.writer->last_status == 406);
    }
}

TEST_CASE("static: the 404 page is served when there is one", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    auto req = make_request(ctx, "/nope");
    REQUIRE(run_on(ctx, site->serve_not_found(req, res)));

    CHECK(writer->last_status == 404);
    CHECK(writer->last_body == "<html>not found</html>");
    CHECK(writer->header("content-type") == "text/html; charset=utf-8");
}

TEST_CASE("static: the 404 is plain text when the site has no page", "[static]") {
    StaticFixture fx;
    fx.write("index.html", "hi");  // deliberately no 404.html
    auto site = load_standard(fx);

    asio::io_context ctx;
    auto writer = std::make_shared<FakeResponseWriter>();
    auto res = std::make_shared<Response>(writer);
    auto req = make_request(ctx, "/nope");
    REQUIRE(run_on(ctx, site->serve_not_found(req, res)));

    CHECK(writer->last_status == 404);
    CHECK(writer->last_body == "404 Not Found");
    CHECK(writer->header("content-type") == "text/plain; charset=utf-8");
}

TEST_CASE("static: no response ever carries a header twice", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.table.immutable_prefixes = {"/assets/"};
    cfg.spa_fallback = "index.html";
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;

    // Response::header() and content_type() both *append*, and this module has
    // six different exit paths each setting five to eight headers. A duplicate is
    // the failure mode that a single path would not show.
    auto check_once = [](const std::shared_ptr<FakeResponseWriter>& w, const char* what) {
        INFO("response kind: " << what);
        for (const auto& [name, value] : w->last_headers) {
            (void)value;
            std::size_t count = 0;
            for (const auto& [other, ignored] : w->last_headers) {
                (void)ignored;
                if (other == name) ++count;
            }
            CHECK(count == 1);
        }
    };

    {
        auto r = get(*site, ctx, "/app.js");
        REQUIRE(r.writer->last_status == 200);
        check_once(r.writer, "200");
    }
    {
        auto r = get(*site, ctx, "/assets/chunk-abc123.js");
        check_once(r.writer, "200 immutable");
    }
    {
        auto req = make_request(ctx, "/app.js");
        const StaticEntry* entry = site->find("/app.js");
        add_header(req, "if-none-match", entry->identity.etag);
        auto r = serve(*site, ctx, req);
        REQUIRE(r.writer->last_status == 304);
        check_once(r.writer, "304");
    }
    {
        auto req = make_request(ctx, "/app.js");
        add_header(req, "range", "bytes=0-2");
        auto r = serve(*site, ctx, req);
        REQUIRE(r.writer->last_status == 206);
        check_once(r.writer, "206");
    }
    {
        auto req = make_request(ctx, "/app.js");
        add_header(req, "range", "bytes=999-");
        auto r = serve(*site, ctx, req);
        REQUIRE(r.writer->last_status == 416);
        check_once(r.writer, "416");
    }
    {
        auto r = serve(*site, ctx, make_request(ctx, "/app.js", Method::Head));
        REQUIRE(r.writer->last_status == 200);
        check_once(r.writer, "HEAD");
    }
    {
        auto r = get(*site, ctx, "/spa/deep/link");
        REQUIRE(r.writer->last_status == 200);
        check_once(r.writer, "SPA fallback");
    }
    {
        auto writer = std::make_shared<FakeResponseWriter>();
        auto res = std::make_shared<Response>(writer);
        auto req = make_request(ctx, "/nope");
        REQUIRE(run_on(ctx, site->serve_not_found(req, res)));
        check_once(writer, "404");
    }
}

// ---------------------------------------------------------------------------
// SPA fallback
// ---------------------------------------------------------------------------

TEST_CASE("static: SPA fallback is off unless a target is configured", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();
    auto site = load_standard(fx);  // no spa_fallback

    asio::io_context ctx;
    auto r = get(*site, ctx, "/spa/deep/link");
    CHECK_FALSE(r.handled);  // a miss, exactly as before
    CHECK(r.writer->last_status == 0);
}

TEST_CASE("static: SPA fallback answers an unmatched path with the index", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.spa_fallback = "index.html";
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;
    auto r = get(*site, ctx, "/spa/deep/link");

    CHECK(r.handled);
    CHECK(r.writer->last_status == 200);
    CHECK(r.writer->last_body == "<html>root index</html>");

    // No validators: a client that cached this URL with the index's ETag would
    // be told 304 later by a URL that is still the fallback, and never see the
    // file once one exists at this path.
    CHECK_FALSE(r.writer->has_header("etag"));
    CHECK_FALSE(r.writer->has_header("last-modified"));
    CHECK(r.writer->header("cache-control") == "no-cache");
}

TEST_CASE("static: SPA fallback leaves extensionless lookups alone", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.spa_fallback = "index.html";
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;

    // A missing script 404s rather than returning HTML under a .js URL: the
    // browser's MIME error would read as a broken build, not a missing file.
    auto js = get(*site, ctx, "/missing.js");
    CHECK_FALSE(js.handled);

    // A path with no dot is a client-side route.
    auto route = get(*site, ctx, "/user/123");
    CHECK(route.handled);
    CHECK(route.writer->last_status == 200);

    // With the guard turned off, even a dotted path falls back.
    StaticFilesConfig any = standard_config(fx);
    any.spa_fallback = "index.html";
    any.spa_fallback_extensionless_only = false;
    auto loose = load_standard(fx, any);
    CHECK(get(*loose, ctx, "/missing.js").handled);
}

TEST_CASE("static: SPA fallback never swallows a reserved prefix", "[static]") {
    StaticFixture fx;
    fx.make_standard_tree();

    StaticFilesConfig cfg = standard_config(fx);
    cfg.spa_fallback = "index.html";
    cfg.reserved_prefixes = {"/api"};
    auto site = load_standard(fx, cfg);

    asio::io_context ctx;

    // The ordering this pins: reserved is checked before the fallback, so an API
    // route is never answered with the index page.
    auto api = get(*site, ctx, "/api/nope");
    CHECK_FALSE(api.handled);
    CHECK(api.writer->last_status == 0);

    // And a route outside the reserved prefix still falls back.
    CHECK(get(*site, ctx, "/elsewhere").handled);
}
