#pragma once

// Commonly used MIME type constants, plus the extension mapping that turns a
// file name into one of them.
//
// The constants let a handler write `res.content_type(mime::text_html)` instead
// of a literal. by_extension is here because "what type is this file" is a
// question every static-content path has to answer, and getting it wrong is
// silent: a missing charset makes the browser guess, and the wrong type on a
// module makes it refuse to execute.

#include <cstddef>
#include <string_view>

namespace simple_http::mime {

inline constexpr std::string_view text_plain = "text/plain";
inline constexpr std::string_view text_html = "text/html";
inline constexpr std::string_view app_json = "application/json";
inline constexpr std::string_view app_xml = "application/xml";
inline constexpr std::string_view app_octet_stream = "application/octet-stream";
inline constexpr std::string_view text_css = "text/css";
inline constexpr std::string_view text_javascript = "text/javascript";
inline constexpr std::string_view image_gif = "image/gif";

// Infers a Content-Type from a path's last extension, falling back to
// application/octet-stream when there is nothing to go on.
//
// Textual types carry `; charset=utf-8` rather than a bare essence. That is part
// of the contract, not decoration: a text response without a charset leaves the
// decoding to the browser, and callers also branch on the prefix —
// handler/static_files.h chooses its cache policy with
// `starts_with("text/html")`. Returning the bare essence would silently change
// that policy.
inline std::string_view by_extension(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return app_octet_stream;
    std::string_view ext = path.substr(dot);

    char lower[16] = {0};
    if (ext.size() >= sizeof(lower)) return app_octet_stream;
    for (std::size_t i = 0; i < ext.size(); ++i) {
        char c = ext[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view e{lower, ext.size()};

    if (e == ".html" || e == ".htm") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs") return "text/javascript; charset=utf-8";
    if (e == ".css") return "text/css; charset=utf-8";
    if (e == ".json" || e == ".map") return "application/json; charset=utf-8";
    if (e == ".xml") return "application/xml; charset=utf-8";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".gif") return "image/gif";
    if (e == ".webp") return "image/webp";
    if (e == ".avif") return "image/avif";
    if (e == ".ico") return "image/x-icon";
    if (e == ".woff2") return "font/woff2";
    if (e == ".woff") return "font/woff";
    if (e == ".ttf") return "font/ttf";
    if (e == ".otf") return "font/otf";
    if (e == ".wasm") return "application/wasm";
    if (e == ".txt") return "text/plain; charset=utf-8";
    if (e == ".md") return "text/markdown; charset=utf-8";
    if (e == ".webmanifest") return "application/manifest+json";
    if (e == ".pdf") return "application/pdf";
    return app_octet_stream;
}

}  // namespace simple_http::mime
