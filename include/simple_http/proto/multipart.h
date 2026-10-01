#pragma once

// multipart/form-data parsing (RFC 7578): splits a request body into parts by
// the boundary carried in its Content-Type, each part carrying its
// Content-Disposition name/filename and its own Content-Type. This is the
// axum `Multipart` / Go `r.ParseMultipartForm` base for file uploads and
// mixed forms.
//
// Parsing is strict where framing matters (a missing boundary or a body that
// never closes returns nullopt) and permissive with the parts themselves
// (headers it does not recognize are ignored). Sizes are bounded per part and
// per count so a peer cannot make the parser buffer without limit.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/types.h" // iequals_ci (header names arrive in original case)

namespace simple_http {

// One parsed part. A plain text field has an empty `filename` and an empty (or
// defaulted) `content_type`; a file upload has `filename` set.
struct MultipartPart {
    std::string name;         // Content-Disposition: form-data; name="..."
    std::string filename;     // ...; filename="..." — empty for a plain field
    std::string content_type; // the part's own Content-Type, "" = text/plain
    std::string data;         // the decoded part body
};

namespace multipart_detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return std::string{s.substr(1, s.size() - 2)};
    }
    return std::string{s};
}

// Fills `part` from the header block of one part: Content-Disposition
// (name/filename) and Content-Type. Lines are separated by CRLF.
inline void parse_part_head(std::string_view head, MultipartPart &part) {
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = head.find('\n', pos);
        std::string_view line = head.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name = trim(line.substr(0, colon));
            std::string_view value = trim(line.substr(colon + 1));
            if (iequals_ci(name, "content-disposition")) {
                // form-data; name="field"; filename="f.txt" — scan the
                // semi-colon-separated parameters.
                while (!value.empty()) {
                    const std::size_t semi = value.find(';');
                    const std::string_view param =
                        trim(value.substr(0, semi == std::string_view::npos ? std::string_view::npos : semi));
                    const std::size_t peq = param.find('=');
                    if (!param.empty() && peq != std::string_view::npos) {
                        std::string pvalue = unquote(trim(param.substr(peq + 1)));
                        const std::string_view pname = trim(param.substr(0, peq));
                        if (pname == "name") {
                            part.name = std::move(pvalue);
                        } else if (pname == "filename") {
                            part.filename = std::move(pvalue);
                        }
                    }
                    if (semi == std::string_view::npos) {
                        break;
                    }
                    value.remove_prefix(semi + 1);
                }
            } else if (iequals_ci(name, "content-type")) {
                part.content_type = std::string{trim(value)};
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
}

} // namespace multipart_detail

// Extracts `boundary="..."` from a "multipart/form-data; boundary=..." value.
// The boundary may be quoted; a boundary that is missing or empty is rejected,
// matching the server side of RFC 7578 §4.1 (a non-empty boundary is required).
inline std::optional<std::string> multipart_boundary(std::string_view content_type) {
    std::size_t pos = 0;
    for (;;) {
        const std::size_t eq = content_type.find('=', pos);
        if (eq == std::string_view::npos) {
            return std::nullopt;
        }
        // Walk back over the parameter's name (the last non-space, non-';' run
        // before '=') and forward over the value's leading whitespace.
        std::size_t name_start = eq;
        while (name_start > pos && content_type[name_start - 1] != ';' && content_type[name_start - 1] != ' ' &&
               content_type[name_start - 1] != '\t') {
            --name_start;
        }
        std::string_view name = content_type.substr(name_start, eq - name_start);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == ';')) {
            name.remove_prefix(1);
        }
        std::string_view value = content_type.substr(eq + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        if (name == "boundary") {
            std::string out;
            if (!value.empty() && value.front() == '"') {
                value.remove_prefix(1);
                const std::size_t quote = value.find('"');
                out.append(value.substr(0, quote == std::string_view::npos ? value.size() : quote));
            } else {
                const std::size_t semi = value.find(';');
                out.append(value.substr(0, semi == std::string_view::npos ? value.size() : semi));
            }
            return out.empty() ? std::nullopt : std::optional<std::string>{std::move(out)};
        }
        pos = eq + 1;
    }
}

// Parses `body` as multipart/form-data with `boundary` (the raw token, without
// the leading "--"). Returns nullopt when the framing is unusable: no opening
// delimiter, a malformed part head, a body that never hits the closing
// delimiter, or a part/count exceeding the caps. `max_part_bytes` bounds a
// single part's data, `max_parts` bounds the part count.
inline std::optional<std::vector<MultipartPart>> parse_multipart(std::string_view body, std::string_view boundary,
                                                                 std::size_t max_part_bytes = 16 * 1024 * 1024,
                                                                 std::size_t max_parts = 100) {
    const std::string delim = "--" + std::string{boundary};
    std::vector<MultipartPart> parts;
    std::size_t pos = 0;
    // RFC 7578 §4.1: an optional CRLF precedes the first boundary.
    if (body.substr(0, 2) == "\r\n") {
        pos = 2;
    }
    const std::string break_delim = "\r\n" + delim;

    for (;;) {
        if (body.substr(pos, delim.size()) != delim) {
            return std::nullopt; // framing lost: no delimiter where one belongs
        }
        pos += delim.size();
        if (body.substr(pos, 2) == "--") {
            break; // closing delimiter
        }
        if (body.substr(pos, 2) != "\r\n") {
            return std::nullopt;
        }
        pos += 2;

        // Part head: field lines until a blank line.
        const std::size_t head_end = body.find("\r\n\r\n", pos);
        if (head_end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view head = body.substr(pos, head_end - pos);
        pos = head_end + 4;

        // Part body: up to the next "\r\n--boundary" (the closing delimiter
        // `--boundary--` has the same break prefix).
        const std::size_t body_end = body.find(break_delim, pos);
        if (body_end == std::string_view::npos) {
            return std::nullopt;
        }
        std::string_view data = body.substr(pos, body_end - pos);
        // The CRLF immediately before the delimiter belongs to the framing, not
        // the part.
        if (data.size() >= 2 && data.substr(data.size() - 2) == "\r\n") {
            data.remove_suffix(2);
        }
        if (data.size() > max_part_bytes) {
            return std::nullopt;
        }

        MultipartPart part;
        multipart_detail::parse_part_head(head, part);
        part.data.assign(data);
        parts.push_back(std::move(part));
        if (parts.size() > max_parts) {
            return std::nullopt;
        }
        // The break was "\r\n" + delimiter; skip the CRLF so `pos` is back at a
        // "--boundary" delimiter for the next iteration.
        pos = body_end + 2;
    }
    return parts;
}

} // namespace simple_http