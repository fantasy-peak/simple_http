module;

#include "simple_http.h"

export module simple_http;

export namespace simple_http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

// HttpServer
using ::simple_http::Config;
using ::simple_http::HttpRequestReader;
using ::simple_http::HttpResponseWriter;
using ::simple_http::HttpServer;
// Http2Client
using ::simple_http::GenericStream;
using ::simple_http::Http2Client;
using ::simple_http::HttpClientConfig;
using ::simple_http::HttpRequestWriter;
using ::simple_http::HttpResponseReader;
using ::simple_http::StreamSpec;
// state
using ::simple_http::Disconnect;
using ::simple_http::Eof;
using ::simple_http::ParseHeaderDone;

using ::simple_http::IoCtxPool;
using ::simple_http::LogLevel;
using ::simple_http::overloaded;
using ::simple_http::Version;
using ::simple_http::WriteMode;

using ::simple_http::RequestCallback;
using ::simple_http::WebSocketCallback;
using ::simple_http::WsCallBack;
using ::simple_http::WssCallBack;
using ::simple_http::WssStreamPtr;
using ::simple_http::WsStreamPtr;
using ::simple_http::WssUnixStreamPtr;
using ::simple_http::WsUnixStreamPtr;

// function
using ::simple_http::base64UrlDecode;
using ::simple_http::base64UrlEncode;
using ::simple_http::makeHttpResponse;
using ::simple_http::toLower;
using ::simple_http::toString;

namespace __private {
using ::__private::SessionContext;
}

using ::simple_http::alpn_proto_list;
using ::simple_http::channel_capacity;
using ::simple_http::LOG_CB;

using ::simple_http::client_version;
using ::simple_http::server_version;
using ::simple_http::version_major;
using ::simple_http::version_minor;
using ::simple_http::version_patch;

namespace mime {
using ::simple_http::mime::app_json;
using ::simple_http::mime::app_octet_stream;
using ::simple_http::mime::app_xml;
using ::simple_http::mime::image_gif;
using ::simple_http::mime::text_css;
using ::simple_http::mime::text_html;
using ::simple_http::mime::text_javascript;
using ::simple_http::mime::text_plain;
}  // namespace mime

}  // namespace simple_http
