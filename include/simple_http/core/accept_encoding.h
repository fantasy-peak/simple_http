#pragma once

// Accept-Encoding parsing: which content codings a client will accept, and at
// what quality, in the form a caller needs to choose between representations it
// already has on hand.
//
// This is not `negotiate_encoding` (core/content_encoding.h). That one answers
// "which coding should *I* produce", and assumes the server is compressing:
// it knows nothing about identity's qvalue and cannot express "the bytes for
// these codings are already on disk, pick one". Giving it an extra parameter for
// that would turn a one-dimensional decision into two and make every existing
// caller care about a case it does not have.
//
// It is also deliberately not part of that header: content_encoding.h is the
// *encoder* layer, with zlib and brotli behind a build macro. A caller that only
// wants to parse a header should not pull in the encoders to do it.
//
// NAMES THIS FILE MUST NOT DEFINE: `trim_ascii`, `starts_with_ci` and
// `parse_qvalue` already exist in core/content_encoding.h, and `iequals_ci` in
// core/types.h. Same-named `inline` functions in one namespace are a
// redefinition error, and the umbrella header pulls both in — so a duplicate
// here fails to compile across the whole library. Reuse them; the case-folding
// used below is `iequals_ci` from types.h.

#include <cstddef>
#include <string>
#include <string_view>

#include "types.h"  // iequals_ci

namespace simple_http {

// Quality values for the codings this library can serve. -1 means "not mentioned
// at all", which is not the same as an explicit 0.0: absence leaves the coding
// acceptable, and a caller resolving a default has to be able to tell the two
// apart.
struct AcceptEncoding {
    double br{-1.0};
    double gzip{-1.0};
    double wildcard{-1.0};  // q of "*", or -1 when absent
    // Also -1 when unmentioned. Identity is acceptable by default (RFC 9110
    // §12.5.3), but "*;q=0" refuses it too — so "unmentioned" and "explicitly
    // 1.0" must be distinguishable, and the caller resolves the default.
    double identity{-1.0};
};

// Parses a qvalue list such as `gzip, deflate, br;q=0.8`.
//
// A plain substring search ("does it mention br") is a real bug rather than a
// simplification: `Accept-Encoding: gzip, br;q=0` says the client REFUSES
// brotli, and answering with a brotli body produces a response the client cannot
// decode.
inline AcceptEncoding parse_accept_encoding(std::string_view header) {
    AcceptEncoding ae;
    std::size_t pos = 0;
    while (pos <= header.size()) {
        auto comma = header.find(',', pos);
        std::string_view item =
            header.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);

        auto semi = item.find(';');
        std::string_view coding = item.substr(0, semi);
        double q = 1.0;
        if (semi != std::string_view::npos) {
            std::string_view params = item.substr(semi + 1);
            auto qpos = params.find('q');
            if (qpos != std::string_view::npos) {
                auto eq = params.find('=', qpos);
                if (eq != std::string_view::npos) {
                    std::string_view v = params.substr(eq + 1);
                    std::string tmp;
                    for (char c : v) {
                        if (c == ' ' || c == '\t') continue;
                        if ((c >= '0' && c <= '9') || c == '.') {
                            tmp.push_back(c);
                        } else {
                            break;
                        }
                    }
                    try {
                        if (!tmp.empty()) q = std::stod(tmp);
                    } catch (const std::exception&) {
                        q = 0.0;
                    }
                }
            }
        }
        // Trim the coding token. Content-coding names are case-insensitive
        // (RFC 9110 §8.4.1), so "GZIP" and "gzip" are the same coding.
        while (!coding.empty() && (coding.front() == ' ' || coding.front() == '\t')) coding.remove_prefix(1);
        while (!coding.empty() && (coding.back() == ' ' || coding.back() == '\t')) coding.remove_suffix(1);

        if (iequals_ci(coding, "br")) {
            ae.br = q;
        } else if (iequals_ci(coding, "gzip") || iequals_ci(coding, "x-gzip")) {
            ae.gzip = q;
        } else if (coding == "*") {
            ae.wildcard = q;
        } else if (iequals_ci(coding, "identity")) {
            ae.identity = q;
        }

        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return ae;
}

// Resolves identity's quality. An explicit `identity;q=` wins outright;
// otherwise `*;q=0` refuses it; otherwise it is acceptable (RFC 9110 §12.5.3).
inline double identity_q(const AcceptEncoding& ae) noexcept {
    if (ae.identity >= 0.0) return ae.identity;
    if (ae.wildcard == 0.0) return 0.0;
    return 1.0;
}

// The quality that applies to `coding` (pass ae.br or ae.gzip): its explicit
// value when the header named it, otherwise the wildcard's, otherwise -1 for
// "not acceptable".
inline double coding_q(double explicit_q, const AcceptEncoding& ae) noexcept {
    if (explicit_q >= 0.0) return explicit_q;
    return ae.wildcard;
}

}  // namespace simple_http
