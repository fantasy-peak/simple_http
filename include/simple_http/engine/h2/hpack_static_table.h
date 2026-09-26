#pragma once

// The HPACK static table (RFC 7541 Appendix A).
//
// A compile-time array of string_views rather than a mutable global of
// std::string pairs: no static initialisation order to worry about, no 124 string
// constructions at load, and an indexed lookup constructs exactly one std::string
// for the caller instead of copying a pair out of a shared table.
//
// Index 0 is the sentinel the RFC reserves ("no index"), so a wire index is
// usable as a table index directly.

#include <array>
#include <string_view>

namespace simple_http::codec {

struct StaticTableEntry {
    std::string_view key;
    std::string_view value;
};

// 0 .. 61 inclusive. The front-loaded block is the RFC's ordering: the request
// and response pseudo-headers first (2-7 and 8-14), then the ordinary fields in
// ASCII order.
inline constexpr std::array<StaticTableEntry, 62> http2_header_static_table{{
    {":empty", ""},                                  // 0 (sentinel, never on the wire)
    {":authority", ""},                              // 1
    {":method", "GET"},                              // 2
    {":method", "POST"},                             // 3
    {":path", "/"},                                  // 4
    {":path", "/index.html"},                        // 5
    {":scheme", "http"},                             // 6
    {":scheme", "https"},                            // 7
    {":status", "200"},                              // 8
    {":status", "204"},                              // 9
    {":status", "206"},                              // 10
    {":status", "304"},                              // 11
    {":status", "400"},                              // 12
    {":status", "404"},                              // 13
    {":status", "500"},                              // 14
    {"accept-charset", ""},                          // 15
    {"accept-encoding", "gzip, deflate"},            // 16
    {"accept-language", ""},                         // 17
    {"accept-ranges", ""},                           // 18
    {"accept", ""},                                  // 19
    {"access-control-allow-origin", ""},             // 20
    {"age", ""},                                     // 21
    {"allow", ""},                                   // 22
    {"authorization", ""},                           // 23
    {"cache-control", ""},                           // 24
    {"content-disposition", ""},                     // 25
    {"content-encoding", ""},                        // 26
    {"content-language", ""},                        // 27
    {"content-length", ""},                          // 28
    {"content-location", ""},                        // 29
    {"content-range", ""},                           // 30
    {"content-type", ""},                            // 31
    {"cookie", ""},                                  // 32
    {"date", ""},                                    // 33
    {"etag", ""},                                    // 34
    {"expect", ""},                                  // 35
    {"expires", ""},                                 // 36
    {"from", ""},                                    // 37
    {"host", ""},                                    // 38
    {"if-match", ""},                                // 39
    {"if-modified-since", ""},                       // 40
    {"if-none-match", ""},                           // 41
    {"if-range", ""},                                // 42
    {"if-unmodified-since", ""},                     // 43
    {"last-modified", ""},                           // 44
    {"link", ""},                                    // 45
    {"location", ""},                                // 46
    {"max-forwards", ""},                            // 47
    {"proxy-authenticate", ""},                      // 48
    {"proxy-authorization", ""},                     // 49
    {"range", ""},                                   // 50
    {"referer", ""},                                 // 51
    {"refresh", ""},                                 // 52
    {"retry-after", ""},                             // 53
    {"server", ""},                                  // 54
    {"set-cookie", ""},                              // 55
    {"strict-transport-security", ""},               // 56
    {"transfer-encoding", ""},                       // 57
    {"user-agent", ""},                              // 58
    {"vary", ""},                                    // 59
    {"via", ""},                                     // 60
    {"www-authenticate", ""},                        // 61
}};

}  // namespace simple_http::codec
