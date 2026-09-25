#pragma once

// HPACK header-block encoding + the HTTP/2 static table.
//
// It keeps only what the encoders need: the HPACK static table (RFC 7541
// Appendix A), the header-name -> index map, and the make_http2_headers_* /
// set_http2_* builders. Depends only on the standard library and the Huffman
// codec (hpack_huffman.h).

#include <charconv>
#include <map>
#include <string>

#include "hpack_huffman.h"  // http_huffman_encode

namespace simple_http::codec {

struct http2_header_static_table_t {
    std::string key;
    std::string value;
};

// HPACK per-header static-table indices, used by the make_*/set_http2_* API.
#define HTTP2_CODE_authority 1
#define HTTP2_CODE_GET 2
#define HTTP2_CODE_POST 3
#define HTTP2_CODE_path 4
#define HTTP2_CODE_index 5
#define HTTP2_CODE_http 6
#define HTTP2_CODE_https 7
#define HTTP2_CODE_200 8
#define HTTP2_CODE_204 9
#define HTTP2_CODE_206 10
#define HTTP2_CODE_304 11
#define HTTP2_CODE_400 12
#define HTTP2_CODE_404 13
#define HTTP2_CODE_500 14
#define HTTP2_CODE_accept_charset 15
#define HTTP2_CODE_accept_encoding 16
#define HTTP2_CODE_accept_language 17
#define HTTP2_CODE_accept_ranges 18
#define HTTP2_CODE_accept 19
#define HTTP2_CODE_access_control_allow_origin 20
#define HTTP2_CODE_age 21
#define HTTP2_CODE_allow 22
#define HTTP2_CODE_authorization 23
#define HTTP2_CODE_cache_control 24
#define HTTP2_CODE_content_disposition 25
#define HTTP2_CODE_content_encoding 26
#define HTTP2_CODE_content_language 27
#define HTTP2_CODE_content_length 28
#define HTTP2_CODE_content_location 29
#define HTTP2_CODE_content_range 30
#define HTTP2_CODE_content_type 31
#define HTTP2_CODE_cookie 32
#define HTTP2_CODE_date 33
#define HTTP2_CODE_etag 34
#define HTTP2_CODE_expect 35
#define HTTP2_CODE_expires 36
#define HTTP2_CODE_from 37
#define HTTP2_CODE_host 38
#define HTTP2_CODE_if_match 39
#define HTTP2_CODE_if_modified_since 40
#define HTTP2_CODE_if_none_match 41
#define HTTP2_CODE_if_range 42
#define HTTP2_CODE_if_unmodified_since 43
#define HTTP2_CODE_last_modified 44
#define HTTP2_CODE_link 45
#define HTTP2_CODE_location 46
#define HTTP2_CODE_max_forwards 47
#define HTTP2_CODE_proxy_authenticate 48
#define HTTP2_CODE_proxy_authorization 49
#define HTTP2_CODE_range 50
#define HTTP2_CODE_referer 51
#define HTTP2_CODE_refresh 52
#define HTTP2_CODE_retry_after 53
#define HTTP2_CODE_server 54
#define HTTP2_CODE_set_cookie 55
#define HTTP2_CODE_strict_transport_security 56
#define HTTP2_CODE_transfer_encoding 57
#define HTTP2_CODE_user_agent 58
#define HTTP2_CODE_vary 59
#define HTTP2_CODE_via 60
#define HTTP2_CODE_www_authenticate 61

// HPACK static table (RFC 7541 Appendix A), index 0 unused/sentinel.
inline http2_header_static_table_t http2_header_static_table[] = {
    {":empty", ""},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

// header name -> static code.
inline std::map<std::string, unsigned char> http2_header_codes_table = {
    {"empty", 0},
    {"authority", 1},
    {"GET", 2},
    {"POST", 3},
    {"path", 4},
    {"index", 5},
    {"http", 6},
    {"https", 7},
    {"200", 8},
    {"204", 9},
    {"206", 10},
    {"304", 11},
    {"400", 12},
    {"404", 13},
    {"500", 14},
    {"accept-charset", 15},
    {"accept-encoding", 16},
    {"accept-language", 17},
    {"accept-ranges", 18},
    {"accept", 19},
    {"access-control-allow-origin", 20},
    {"age", 21},
    {"allow", 22},
    {"authorization", 23},
    {"cache-control", 24},
    {"content-disposition", 25},
    {"content-encoding", 26},
    {"content-language", 27},
    {"content-length", 28},
    {"content-location", 29},
    {"content-range", 30},
    {"content-type", 31},
    {"cookie", 32},
    {"date", 33},
    {"etag", 34},
    {"expect", 35},
    {"expires", 36},
    {"from", 37},
    {"host", 38},
    {"if-match", 39},
    {"if-modified-since", 40},
    {"if-none-match", 41},
    {"if-range", 42},
    {"if-unmodified-since", 43},
    {"last-modified", 44},
    {"link", 45},
    {"location", 46},
    {"max-forwards", 47},
    {"proxy-authenticate", 48},
    {"proxy-authorization", 49},
    {"range", 50},
    {"referer", 51},
    {"refresh", 52},
    {"retry-after", 53},
    {"server", 54},
    {"set-cookie", 55},
    {"strict-transport-security", 56},
    {"transfer-encoding", 57},
    {"user-agent", 58},
    {"vary", 59},
    {"via", 60},
    {"www-authenticate", 61},
};

// --- HTTP/2 header/frame builders ---
bool make_http2_headers(std::string &hh_data, unsigned int streamid);
bool set_http2_headers_static(unsigned char *hh_data, unsigned char hh_code);
bool make_http2_headers_static(std::string &hh_data, unsigned int hh_code);

bool make_http2_headers_item(std::string &hh_data, unsigned char hh_code);
bool make_http2_headers_item(std::string &hh_data, unsigned char, const std::string &value);

bool make_http2_headers_item2(std::string &hh_data, unsigned char, const std::string &value);
bool make_http2_headers_item2(std::string &hh_data, const std::string &key, const std::string &value);
bool make_http2_headers_item2(std::string &hh_data, unsigned char, unsigned long long num);

bool make_http2_headers_item3(std::string &hh_data, const std::string &key, const std::string &value);
bool make_http2_headers_item3(std::string &hh_data, unsigned char, const std::string &value);
bool make_http2_headers_item3(std::string &hh_data, unsigned char, unsigned long long num);

bool make_http2_headers_item4(std::string &hh_data, const std::string &key, const std::string &value);
bool make_http2_headers_item4(std::string &hh_data, unsigned char, const std::string &value);
bool make_http2_headers_item4(std::string &hh_data, unsigned char, unsigned long long num);

bool set_http2_frame_streamid(std::string &hh_data, unsigned int streamid);
bool set_http2_headers_size(std::string &hh_data, unsigned int sizenum);
bool set_http2_headers_flag(std::string &hh_data, unsigned char flag);

// --- inline definitions ---

inline bool make_http2_headers(std::string &hh_data, unsigned int streamid)
{
    unsigned char frame_header_data[] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 9; i++)
    {
        hh_data.push_back(frame_header_data[i]);
    }
    frame_header_data[8] = streamid & 0xFF;
    streamid             = streamid >> 8;
    frame_header_data[7] = streamid & 0xFF;
    streamid             = streamid >> 8;
    frame_header_data[6] = streamid & 0xFF;
    streamid             = streamid >> 8;
    frame_header_data[5] = streamid & 0xFF;
    return true;
}
inline bool set_http2_frame_streamid(std::string &hh_data, unsigned int streamid)
{
    if (hh_data.size() < 9)
    {
        return false;
    }
    hh_data[8] = streamid & 0xFF;
    streamid   = streamid >> 8;
    hh_data[7] = streamid & 0xFF;
    streamid   = streamid >> 8;
    hh_data[6] = streamid & 0xFF;
    streamid   = streamid >> 8;
    hh_data[5] = streamid & 0xFF;
    return true;
}
inline bool set_http2_headers_size(std::string &hh_data, unsigned int sizenum)
{
    if (hh_data.size() < 3)
    {
        return false;
    }
    hh_data[2] = sizenum & 0xFF;
    sizenum    = sizenum >> 8;
    hh_data[1] = sizenum & 0xFF;
    sizenum    = sizenum >> 8;
    hh_data[0] = sizenum & 0xFF;
    return true;
}
inline bool set_http2_headers_flag(std::string &hh_data, unsigned char flag)
{
    if (hh_data.size() < 9)
    {
        return false;
    }
    hh_data[4] = flag;
    return true;
}
inline bool set_http2_headers_static(unsigned char *hh_data, unsigned char hh_code)
{
    *hh_data = hh_code | 0x80;
    return true;
}
inline bool make_http2_headers_static(std::string &hh_data, unsigned int hh_code)
{
    switch (hh_code)
    {
    case 200:
        hh_data.push_back((char)0x88);
        break;
    case 204:
        hh_data.push_back((char)0x89);
        break;
    case 206:
        hh_data.push_back((char)0x8A);
        break;
    case 302:
        hh_data.push_back(0x48);
        hh_data.push_back(0x03);
        hh_data.push_back(0x33);
        hh_data.push_back(0x30);
        hh_data.push_back(0x32);
        break;
    case 304:
        hh_data.push_back((char)0x8B);
        break;
    case 400:
        hh_data.push_back((char)0x8C);
        break;
    case 403:
        hh_data.push_back(0x48);
        hh_data.push_back(0x03);
        hh_data.push_back(0x34);
        hh_data.push_back(0x30);
        hh_data.push_back(0x33);
        break;
    case 404:
        hh_data.push_back((char)0x8D);
        break;
    case 500:
        hh_data.push_back((char)0x8E);
        break;
    default:
        hh_data.push_back((char)0x88);
        return false;
    }
    return true;
}
inline bool make_http2_headers_item(std::string &hh_data, unsigned char hh_code)
{
    hh_data.push_back(hh_code | 0x80);
    return true;
}
inline bool make_http2_headers_item(std::string &hh_data, unsigned char hh_code, const std::string &value)
{
    std::string en_value;
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    hh_data.push_back(hh_code | 0x40);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.push_back((unsigned char)en_value.size() | 0x80);
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
inline bool make_http2_headers_item2(std::string &hh_data, const std::string &key, const std::string &value)
{
    hh_data.push_back(0x40);
    std::string en_value;
    http_huffman_encode((unsigned char *)&key[0], key.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    en_value.clear();
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
inline bool make_http2_headers_item2(std::string &hh_data, unsigned char hh_code, const std::string &value)
{
    std::string en_value;
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (hh_code > 63)
    {
        hh_data.push_back(0x7F);
        hh_data.push_back(hh_code - 63);
    }
    else
    {
        hh_data.push_back(hh_code | 0x40);
    }
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.push_back((unsigned char)en_value.size() | 0x80);
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
// Huffman-encodes `num` in decimal and appends it length-prefixed to `out`.
// The integer is formatted into a stack buffer rather than through
// std::to_string, which would heap-allocate on every encoded header. The length
// prefix always carries the Huffman flag (0x80), as every value here is encoded.
inline void hpack_append_huffman_int(std::string &out, unsigned long long num)
{
    char buf[24];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), num);
    std::string en_value;
    http_huffman_encode(reinterpret_cast<unsigned char *>(buf), static_cast<unsigned int>(end - buf), en_value);
    out.push_back((unsigned char)en_value.size() | 0x80);
    out.append(en_value.data(), en_value.size());
}

inline bool make_http2_headers_item2(std::string &hh_data, unsigned char hh_code, unsigned long long num)
{
    if (hh_code > 63)
    {
        hh_data.push_back(0x7F);
        hh_data.push_back(hh_code - 63);
    }
    else
    {
        hh_data.push_back(hh_code | 0x40);
    }
    hpack_append_huffman_int(hh_data, num);
    return true;
}
inline bool make_http2_headers_item3(std::string &hh_data, const std::string &key, const std::string &value)
{
    hh_data.push_back(0x00);
    std::string en_value;
    http_huffman_encode((unsigned char *)&key[0], key.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    en_value.clear();
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
inline bool make_http2_headers_item3(std::string &hh_data, unsigned char hh_code, const std::string &value)
{
    std::string en_value;
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (hh_code > 15)
    {
        hh_data.push_back(0x0F);
        hh_data.push_back(hh_code - 15);
    }
    else
    {
        hh_data.push_back(hh_code | 0x00);
    }
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
inline bool make_http2_headers_item3(std::string &hh_data, unsigned char hh_code, unsigned long long num)
{
    if (hh_code > 15)
    {
        hh_data.push_back(0x0F);
        hh_data.push_back(hh_code - 15);
    }
    else
    {
        hh_data.push_back(hh_code | 0x00);
    }
    hpack_append_huffman_int(hh_data, num);
    return true;
}
inline bool make_http2_headers_item4(std::string &hh_data, unsigned char hh_code, const std::string &value)
{
    std::string en_value;
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (hh_code > 15)
    {
        hh_data.push_back(0x1F);
        hh_data.push_back(hh_code - 15);
    }
    else
    {
        hh_data.push_back(hh_code | 0x10);
    }
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}
inline bool make_http2_headers_item4(std::string &hh_data, unsigned char hh_code, unsigned long long num)
{
    if (hh_code > 15)
    {
        hh_data.push_back(0x1F);
        hh_data.push_back(hh_code - 15);
    }
    else
    {
        hh_data.push_back(hh_code | 0x10);
    }
    hpack_append_huffman_int(hh_data, num);
    return true;
}
inline bool make_http2_headers_item4(std::string &hh_data, const std::string &key, const std::string &value)
{
    hh_data.push_back(0x10);
    std::string en_value;
    http_huffman_encode((unsigned char *)&key[0], key.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    en_value.clear();
    http_huffman_encode((unsigned char *)&value[0], value.size(), en_value);
    if (en_value.size() > 0x7F)
    {
        unsigned int totalsize = en_value.size() - 0x7F;
        hh_data.push_back((char)0xFF);
        if (totalsize > 127)
        {
            unsigned char temp    = totalsize / 128;
            unsigned char tempsub = totalsize % 128;
            hh_data.push_back((unsigned char)tempsub | 0x80);
            hh_data.push_back((unsigned char)temp);
        }
        else
        {
            hh_data.push_back((unsigned char)totalsize);
        }
    }
    else
    {
        hh_data.push_back((unsigned char)en_value.size() | 0x80);
    }
    hh_data.append((char *)&en_value[0], en_value.size());
    return true;
}

}  // namespace simple_http::codec
