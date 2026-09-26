#pragma once

// A throwaway document root on disk, for the static-file tests.
//
// Both test binaries use it: the unit tests drive StaticFiles directly, and
// server_regression.cpp drives it over a real socket. It lives in test/ rather
// than test/unit/ because the regression target compiles from a different
// directory and reaches it as "static_fixture.h".
//
// Everything is torn down in the destructor, so a test can run repeatedly and
// two tests can run in parallel without colliding: the directory name carries
// the process id and a per-instance counter.
//
// The tree is deliberately hostile — a symlink to a file outside the root, a
// symlink to a directory outside it, and a FIFO. Those are the shapes that make
// "the request path never touches the filesystem" a property worth testing
// rather than asserting in a comment.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>  // mkfifo
#endif

namespace simple_http::test {

class StaticFixture {
  public:
    StaticFixture() {
        static std::atomic<unsigned> counter{0};
        m_root = std::filesystem::temp_directory_path() /
                 ("simple_http_static_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    ~StaticFixture() {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);  // best-effort
    }

    StaticFixture(const StaticFixture&) = delete;
    StaticFixture& operator=(const StaticFixture&) = delete;

    const std::filesystem::path& root() const { return m_root; }
    std::string root_string() const { return m_root.string(); }

    // Writes `content` to `rel` under the root, creating parents.
    void write(std::string_view rel, std::string_view content) {
        const auto path = m_root / rel;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    void mkdir(std::string_view rel) { std::filesystem::create_directories(m_root / rel); }

#ifndef _WIN32
    // A symlink pointing anywhere — the escape the scanner must refuse to follow.
    void symlink(std::string_view rel, const std::filesystem::path& target) {
        std::filesystem::create_symlink(target, m_root / rel);
    }

    // A named pipe: admitting one to the table (or opening it) would hang.
    void fifo(std::string_view rel) { ::mkfifo((m_root / rel).c_str(), 0644); }
#endif

    // The standard tree. Every test that does not need something unusual uses
    // this, so the expectations below are shared rather than repeated.
    void make_standard_tree() {
        write("index.html", "<html>root index</html>");
        write("404.html", "<html>not found</html>");
        write("app.js", "console.log('app');");
        write("app.js.br", "BROTLI:console.log('app');");
        write("app.js.gz", "GZIP:console.log('app');");
        write("empty.txt", "");
        write("big.bin", std::string(4096, 'x'));
        write("blog/index.html", "<html>blog</html>");
        write("blog/post.html", "<html>post</html>");
        write("assets/chunk-abc123.js", "console.log('chunk');");
        write(".hidden", "dotfile");
#ifndef _WIN32
        symlink("escape", "/etc/passwd");
        symlink("deep", m_root.parent_path());
        fifo("pipe");
#endif
    }

  private:
    std::filesystem::path m_root;
};

}  // namespace simple_http::test
