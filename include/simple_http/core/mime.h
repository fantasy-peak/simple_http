#pragma once

// A small set of commonly used MIME type constants.

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

}  // namespace simple_http::mime
