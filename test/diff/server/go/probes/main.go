// Go client probes for the server differential.
//
// Each group is built into its own binary; every probe sends the same request
// to the C++ reference server and to the Go reference server and prints two
// JSON observations per case (target=cpp and target=go). The runner pairs them
// and diffs the fields.
package main

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/cookiejar"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/gorilla/websocket"
	"github.com/quic-go/quic-go/http3"
	"golang.org/x/net/http2"
)

var defaultGroup = "routing"

type caseSpec struct {
	ID         string
	Protocol   string // h1 | h2c | tls | h3
	Method     string
	Path       string
	Headers    map[string]string
	Host       string
	Body       []byte
	Chunked    bool
	Trailers   map[string]string
	Decompress bool
	Compare    string // optional note for the runner
}

type observation struct {
	Case         string              `json:"case"`
	Target       string              `json:"target"`
	Mode         string              `json:"mode,omitempty"`
	Status       *int                `json:"status,omitempty"`
	Version      string              `json:"version"`
	Headers      map[string][]string `json:"headers"`
	Body         string              `json:"body"`
	BodyEncoding string              `json:"body_encoding"`
	Framing      string              `json:"framing,omitempty"`
	Error        *string             `json:"error,omitempty"`
	Notes        map[string]string   `json:"notes,omitempty"`
}

func emit(obs observation) {
	data, err := json.Marshal(obs)
	if err != nil {
		fmt.Fprintf(os.Stderr, "marshal: %v\n", err)
		return
	}
	fmt.Println(string(data))
}

func baseFor(target, protocol, cppH1, cppH2C, cppTLS, cppH3, cppMTLS, goH1, goH2C, goTLS, goH3,
	goMTLS string) string {
	if target == "cpp" {
		switch protocol {
		case "h2c":
			return cppH2C
		case "tls":
			return cppTLS
		case "h3":
			return cppH3
		case "mtls", "mtls-nocert":
			return cppMTLS
		default:
			return cppH1
		}
	}
	switch protocol {
	case "h2c":
		return goH2C
	case "tls":
		return goTLS
	case "h3":
		return goH3
	case "mtls", "mtls-nocert":
		return goMTLS
	default:
		return goH1
	}
}

func clientFor(protocol string, jar http.CookieJar) *http.Client {
	var transport http.RoundTripper
	switch protocol {
	case "h2c":
		transport = &http2.Transport{
			AllowHTTP: true,
			DialTLSContext: func(ctx context.Context, network, addr string,
				cfg *tls.Config) (net.Conn, error) {
				var dialer net.Dialer
				return dialer.DialContext(ctx, network, addr)
			},
		}
	case "tls":
		transport = &http.Transport{
			TLSClientConfig:   &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12},
			ForceAttemptHTTP2: true,
		}
	case "h3":
		transport = &http3.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13},
		}
	case "mtls", "mtls-nocert":
		cfg := &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12}
		if protocol == "mtls" {
			// The test cert has no SAN, so verification is off; what is under
			// test is the server's client-certificate requirement.
			cert, err := tls.LoadX509KeyPair("./test/tls_certificates/client_cert.pem",
				"./test/tls_certificates/client_key.pem")
			if err == nil {
				cfg.Certificates = []tls.Certificate{cert}
			}
		}
		transport = &http.Transport{TLSClientConfig: cfg}
	default:
		plain := http.DefaultTransport.(*http.Transport).Clone()
		plain.DisableCompression = true
		transport = plain
	}
	return &http.Client{
		Transport: transport,
		Timeout:   10 * time.Second,
		Jar:       jar,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}

func classify(err error) string {
	if err == nil {
		return ""
	}
	var netErr net.Error
	if errors.As(err, &netErr) && netErr.Timeout() {
		return "timeout"
	}
	if errors.Is(err, context.DeadlineExceeded) || os.IsTimeout(err) {
		return "timeout"
	}
	text := err.Error()
	if strings.Contains(text, "connection refused") || strings.Contains(text, "connect:") {
		return "connect"
	}
	// Both reference servers reject a missing client certificate with the TLS
	// "certificate required" alert, so the handshake-failure slug is shared.
	if strings.Contains(text, "certificate required") || strings.Contains(text, "bad certificate") {
		return "client_cert_required"
	}
	return "other"
}

func framingOf(resp *http.Response) string {
	for _, encoding := range resp.TransferEncoding {
		if strings.EqualFold(encoding, "chunked") {
			return "chunked"
		}
	}
	if resp.ContentLength >= 0 {
		return "content-length"
	}
	return "none"
}

func protoName(proto string) string {
	switch {
	case strings.HasPrefix(proto, "HTTP/2"):
		return "HTTP/2"
	case strings.HasPrefix(proto, "HTTP/3"):
		return "HTTP/3"
	default:
		return proto
	}
}

func gzipBytes(data []byte) []byte {
	var buffer bytes.Buffer
	writer := gzip.NewWriter(&buffer)
	_, _ = writer.Write(data)
	_ = writer.Close()
	return buffer.Bytes()
}

func buildRequest(base string, spec caseSpec) (*http.Request, error) {
	target := spec.Path
	if target == "" {
		target = "/"
	}
	method := spec.Method
	if method == "" {
		method = http.MethodGet
	}
	var body io.Reader
	if spec.Body != nil {
		body = bytes.NewReader(spec.Body)
	}
	request, err := http.NewRequest(method, base+target, body)
	if err != nil {
		return nil, err
	}
	if spec.Chunked {
		request.ContentLength = -1
	}
	if spec.Host != "" {
		request.Host = spec.Host
	}
	for name, value := range spec.Headers {
		request.Header.Set(name, value)
	}
	if len(spec.Trailers) > 0 {
		request.Trailer = http.Header{}
		for name, value := range spec.Trailers {
			request.Trailer.Set(name, value)
		}
	}
	request.Header.Set("User-Agent", "diff-server-probe")
	return request, nil
}

func observe(resp *http.Response, err error, decompress bool) observation {
	obs := observation{Headers: map[string][]string{}, BodyEncoding: "utf8"}
	if err != nil {
		reason := classify(err)
		obs.Error = &reason
		return obs
	}
	defer resp.Body.Close()
	status := resp.StatusCode
	obs.Status = &status
	obs.Version = protoName(resp.Proto)
	obs.Framing = framingOf(resp)
	for name, values := range resp.Header {
		obs.Headers[strings.ToLower(name)] = values
	}
	data, readErr := io.ReadAll(resp.Body)
	if readErr != nil {
		reason := classify(readErr)
		obs.Error = &reason
		return obs
	}
	if decompress && strings.Contains(resp.Header.Get("Content-Encoding"), "gzip") {
		reader, err := gzip.NewReader(bytes.NewReader(data))
		if err == nil {
			if plain, err := io.ReadAll(reader); err == nil {
				data = plain
			}
			_ = reader.Close()
		}
	}
	if utf8.Valid(data) {
		obs.Body = string(data)
	} else {
		obs.Body = base64.StdEncoding.EncodeToString(data)
		obs.BodyEncoding = "base64"
	}
	if len(resp.Trailer) > 0 {
		obs.Notes = map[string]string{}
		for name, values := range resp.Trailer {
			obs.Notes["trailer."+strings.ToLower(name)] = strings.Join(values, ",")
		}
	}
	return obs
}

type harness struct {
	cppH1, cppH2C, cppTLS, cppH3, cppMTLS string
	goH1, goH2C, goTLS, goH3, goMTLS      string
	cppRaw, goRaw                         string
}

func (h harness) rawBase(target string) string {
	if target == "cpp" {
		return h.cppRaw
	}
	return h.goRaw
}

func (h harness) base(target string, spec caseSpec) string {
	return baseFor(target, spec.Protocol, h.cppH1, h.cppH2C, h.cppTLS, h.cppH3, h.cppMTLS,
		h.goH1, h.goH2C, h.goTLS, h.goH3, h.goMTLS)
}

func (h harness) run(spec caseSpec) {
	for _, target := range []string{"cpp", "go"} {
		client := clientFor(spec.Protocol, nil)
		request, err := buildRequest(h.base(target, spec), spec)
		obs := observation{}
		if err != nil {
			reason := "other"
			obs = observation{Error: &reason}
		} else {
			resp, err := client.Do(request)
			obs = observe(resp, err, spec.Decompress)
		}
		obs.Case = spec.ID
		obs.Target = target
		emit(obs)
	}
}

// ---------------------------------------------------------------------------
// groups
// ---------------------------------------------------------------------------

func routingCases() []caseSpec {
	return []caseSpec{
		{ID: "routing.exact", Method: "GET", Path: "/plain"},
		{ID: "routing.template", Method: "GET", Path: "/users/42"},
		{ID: "routing.template_invalid", Method: "GET", Path: "/users/abc"},
		{ID: "routing.catchall", Method: "GET", Path: "/files/a/b/c"},
		{ID: "routing.exact_before_template", Method: "GET", Path: "/files/static"},
		{ID: "routing.template_single", Method: "GET", Path: "/files/7"},
		{ID: "routing.regex", Method: "GET", Path: "/api/x/y"},
		{ID: "routing.method_get", Method: "GET", Path: "/method"},
		{ID: "routing.method_post", Method: "POST", Path: "/method"},
		{ID: "routing.method_not_allowed", Method: "DELETE", Path: "/method"},
		{ID: "routing.options", Method: "OPTIONS", Path: "/method"},
		{ID: "routing.head", Method: "HEAD", Path: "/plain"},
		{ID: "routing.host_root", Method: "GET", Path: "/who", Host: "example.com"},
		{ID: "routing.host_api", Method: "GET", Path: "/who", Host: "api.example.com"},
		{ID: "routing.host_wildcard", Method: "GET", Path: "/who", Host: "app.example.com"},
		{ID: "routing.host_port", Method: "GET", Path: "/who", Host: "api.example.com:8080"},
		{ID: "routing.host_lookalike", Method: "GET", Path: "/who", Host: "evil-example.com"},
		{ID: "routing.fallback", Method: "GET", Path: "/definitely-not-a-route.json"},
		{ID: "routing.query_duplicate", Method: "GET", Path: "/list?q=a&q=b&page=2"},
	}
}

func bodyCases() []caseSpec {
	return []caseSpec{
		{ID: "body.json_valid", Method: "POST", Path: "/json",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{"name":"book","age":3}`)},
		{ID: "body.json_invalid", Method: "POST", Path: "/json",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{bad`)},
		{ID: "body.json_type_mismatch", Method: "POST", Path: "/json",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{"name":123,"age":3}`)},
		{ID: "body.form", Method: "POST", Path: "/form",
			Headers: map[string]string{"Content-Type": "application/x-www-form-urlencoded"},
			Body:    []byte("name=book&age=3")},
		{ID: "body.multipart_form", Method: "POST", Path: "/multipart",
			Headers: map[string]string{"Content-Type": "multipart/form-data; boundary=diff-boundary"},
			Body: []byte("--diff-boundary\r\n" +
				"Content-Disposition: form-data; name=\"name\"\r\n\r\nbook\r\n" +
				"--diff-boundary\r\n" +
				"Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n" +
				"Content-Type: text/plain\r\n\r\nhello\r\n" +
				"--diff-boundary--\r\n")},
		{ID: "body.drain_1m", Method: "POST", Path: "/drain", Body: bytes.Repeat([]byte("x"), 1<<20)},
		// Ask both servers to decide before the body is written. Without Expect,
		// an instrumented server may reset the socket while the 2 MiB body is
		// still in flight, which looks like a client-side error instead of 413.
		{ID: "body.too_large", Method: "POST", Path: "/drain",
			Headers: map[string]string{"Expect": "100-continue"},
			Body:    bytes.Repeat([]byte("x"), 2<<20)},
		{ID: "body.chunked_echo", Method: "POST", Path: "/echo", Body: []byte("chunked body"), Chunked: true},
		{ID: "body.expect_ok", Method: "POST", Path: "/expect-ok",
			Headers: map[string]string{"Expect": "100-continue"}, Body: []byte("hello!")},
		{ID: "body.expect_reject", Method: "POST", Path: "/expect-reject",
			Headers: map[string]string{"Expect": "100-continue"}, Body: []byte("body")},
		{ID: "body.request_trailers", Method: "POST", Path: "/trailer-echo",
			Body: []byte("body"), Chunked: true,
			Trailers: map[string]string{"X-Check": "yes"}},
		{ID: "body.response_trailers", Method: "GET", Path: "/trailers"},
		// HTTP/2 keeps a different framing path for trailers, 100-continue and
		// flow control, so the same behaviors run again over h2c.
		{ID: "body.request_trailers_h2", Method: "POST", Path: "/trailer-echo",
			Body: []byte("body"), Chunked: true, Protocol: "h2c",
			Trailers: map[string]string{"X-Check": "yes"}},
		{ID: "body.response_trailers_h2", Method: "GET", Path: "/trailers", Protocol: "h2c"},
		{ID: "body.expect_ok_h2", Method: "POST", Path: "/expect-ok", Protocol: "h2c",
			Headers: map[string]string{"Expect": "100-continue"}, Body: []byte("hello!")},
		{ID: "body.large_1m_h2", Method: "POST", Path: "/drain", Protocol: "h2c",
			Body: bytes.Repeat([]byte("x"), 1<<20)},
		// HTTP/3: response trailers and a body large enough to cross the initial
		// QUIC flow-control window.
		{ID: "body.response_trailers_h3", Method: "GET", Path: "/trailers", Protocol: "h3"},
		{ID: "body.large_1m_h3", Method: "POST", Path: "/drain", Protocol: "h3",
			Body: bytes.Repeat([]byte("x"), 1<<20)},
		{ID: "body.gzip_request", Method: "POST", Path: "/decode",
			Headers: map[string]string{"Content-Encoding": "gzip"},
			Body:    gzipBytes([]byte("hello decompressed"))},
		{ID: "body.deflate_request", Method: "POST", Path: "/decode",
			Headers: map[string]string{"Content-Encoding": "deflate"}, Body: []byte("x")},
		{ID: "body.bad_gzip", Method: "POST", Path: "/decode",
			Headers: map[string]string{"Content-Encoding": "gzip"}, Body: []byte("not gzip")},
		{ID: "body.gzip_over_limit", Method: "POST", Path: "/decode",
			Headers: map[string]string{"Content-Encoding": "gzip"},
			Body:    gzipBytes(bytes.Repeat([]byte("x"), 2<<20))},
	}
}

func semanticsCases() []caseSpec {
	return []caseSpec{
		{ID: "semantics.status_201", Method: "GET", Path: "/status/201"},
		{ID: "semantics.status_204", Method: "GET", Path: "/status/204"},
		{ID: "semantics.status_404", Method: "GET", Path: "/status/404"},
		{ID: "semantics.redirect", Method: "GET", Path: "/redirect/302"},
		{ID: "semantics.stream", Method: "GET", Path: "/stream"},
		{ID: "semantics.sse", Method: "GET", Path: "/sse"},
		{ID: "semantics.gzip", Method: "GET", Path: "/gzip",
			Headers: map[string]string{"Accept-Encoding": "gzip"}, Decompress: true},
		{ID: "semantics.h1_echo", Method: "POST", Path: "/echo", Body: []byte("over h1")},
		{ID: "semantics.h2_echo", Method: "POST", Path: "/echo", Body: []byte("over h2"), Protocol: "h2c"},
		{ID: "semantics.tls_echo", Method: "POST", Path: "/echo", Body: []byte("over tls"), Protocol: "tls"},
		{ID: "semantics.h3_echo", Method: "POST", Path: "/echo", Body: []byte("over h3"), Protocol: "h3"},
		{ID: "semantics.proxy", Method: "GET", Path: "/proxy"},
	}
}

func middlewareCases() []caseSpec {
	return []caseSpec{
		{ID: "middleware.cors_allowed", Method: "GET", Path: "/cors",
			Headers: map[string]string{"Origin": "https://app.example.com"}},
		{ID: "middleware.cors_lookalike", Method: "GET", Path: "/cors",
			Headers: map[string]string{"Origin": "https://evil-example.com"}},
		{ID: "middleware.cors_preflight", Method: "OPTIONS", Path: "/cors",
			Headers: map[string]string{
				"Origin":                         "https://app.example.com",
				"Access-Control-Request-Method":  "POST",
				"Access-Control-Request-Headers": "x-test",
			}},
		{ID: "middleware.secure_plain", Method: "GET", Path: "/secure"},
		{ID: "middleware.secure_tls", Method: "GET", Path: "/secure", Protocol: "tls"},
		{ID: "middleware.basic_missing", Method: "GET", Path: "/basic"},
		{ID: "middleware.basic_ok", Method: "GET", Path: "/basic",
			Headers: map[string]string{"Authorization": "Basic dXNlcjpwYXNz"}},
		{ID: "middleware.request_id", Method: "GET", Path: "/request-id",
			Headers: map[string]string{"X-Request-Id": "fixed-id"}},
		{ID: "middleware.gzip", Method: "GET", Path: "/gzip",
			Headers: map[string]string{"Accept-Encoding": "gzip"}, Decompress: true},
	}
}

func frameworkCases() []caseSpec {
	return []caseSpec{
		{ID: "framework.state", Method: "GET", Path: "/state"},
		{ID: "framework.path", Method: "GET", Path: "/users/7"},
		{ID: "framework.query", Method: "GET", Path: "/list?q=x&page=2"},
		{ID: "framework.json", Method: "POST", Path: "/json",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{"name":"book","age":3}`)},
		{ID: "framework.valid_ok", Method: "POST", Path: "/valid",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{"name":"a","age":3}`)},
		{ID: "framework.valid_fail", Method: "POST", Path: "/valid",
			Headers: map[string]string{"Content-Type": "application/json"},
			Body:    []byte(`{"name":"","age":0}`)},
		{ID: "framework.form", Method: "POST", Path: "/form",
			Headers: map[string]string{"Content-Type": "application/x-www-form-urlencoded"},
			Body:    []byte("name=book&age=3")},
		{ID: "framework.header_ok", Method: "GET", Path: "/header",
			Headers: map[string]string{"X-Page": "3", "Cookie": "sid=abc"}},
		{ID: "framework.header_missing", Method: "GET", Path: "/header"},
		{ID: "framework.host_api", Method: "GET", Path: "/who", Host: "api.example.com"},
		// The CSRF middleware's exempt_paths route: no token is required.
		{ID: "framework.csrf_exempt", Method: "POST", Path: "/csrf/exempt"},
		// Mutual TLS: the verified client CN on the mTLS listener, and the
		// handshake failure when no certificate is presented.
		{ID: "framework.client_cert", Method: "GET", Path: "/client-cert", Protocol: "mtls"},
		{ID: "framework.client_cert_missing", Method: "GET", Path: "/client-cert", Protocol: "mtls-nocert"},
	}
}

func staticCases() []caseSpec {
	return []caseSpec{
		{ID: "static.index", Method: "GET", Path: "/"},
		{ID: "static.file", Method: "GET", Path: "/data.txt"},
		{ID: "static.spa", Method: "GET", Path: "/user/123"},
		{ID: "static.missing_asset", Method: "GET", Path: "/missing.js"},
		{ID: "static.range", Method: "GET", Path: "/data.txt",
			Headers: map[string]string{"Range": "bytes=0-4"}},
		{ID: "static.precompressed", Method: "GET", Path: "/data.txt",
			Headers: map[string]string{"Accept-Encoding": "gzip"}, Decompress: true},
		{ID: "static.precompressed_br", Method: "GET", Path: "/data.txt",
			Headers: map[string]string{"Accept-Encoding": "br"}},
	}
}

func wsObservation(caseName, target, text string, data []byte, err error) {
	obs := observation{Case: caseName, Target: target, Headers: map[string][]string{},
		BodyEncoding: "utf8"}
	if err != nil {
		reason := "other"
		obs.Error = &reason
		emit(obs)
		return
	}
	if text != "" {
		obs.Body = text
	} else if utf8.Valid(data) {
		obs.Body = string(data)
	} else {
		obs.Body = base64.StdEncoding.EncodeToString(data)
		obs.BodyEncoding = "base64"
	}
	emit(obs)
}

// Origin-gated WebSocket route: allowed / denied / missing Origin. The
// handshake status is the observable; 403 bodies are framework-specific.
func runWSOrigin(target, base string) {
	url := strings.Replace(base, "http://", "ws://", 1) + "/ws/guarded"
	record := func(caseName string, resp *http.Response, err error) {
		obs := observation{Case: caseName, Target: target, Headers: map[string][]string{}, BodyEncoding: "utf8"}
		if resp != nil {
			status := resp.StatusCode
			obs.Status = &status
		}
		if err != nil {
			reason := "other"
			obs.Error = &reason
		}
		emit(obs)
	}
	dialer := websocket.Dialer{HandshakeTimeout: 3 * time.Second}
	for _, probe := range []struct {
		caseName string
		origin   string
	}{
		{"ws.origin_allowed", "https://app.example.com"},
		{"ws.origin_denied", "https://evil.example.com"},
		{"ws.origin_missing", ""},
	} {
		var header http.Header
		if probe.origin != "" {
			header = http.Header{"Origin": {probe.origin}}
		}
		conn, resp, err := dialer.Dial(url, header)
		if conn != nil {
			_ = conn.Close()
		}
		record(probe.caseName, resp, err)
	}
}

func runWS(target, base string) {
	url := strings.Replace(base, "http://", "ws://", 1)
	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/echo", nil)
	if err != nil {
		wsObservation("ws.text", target, "", nil, err)
		wsObservation("ws.binary", target, "", nil, err)
		wsObservation("ws.large", target, "", nil, err)
		return
	}
	defer conn.Close()
	if err := conn.WriteMessage(websocket.TextMessage, []byte("hello")); err != nil {
		wsObservation("ws.text", target, "", nil, err)
	} else {
		_, data, err := conn.ReadMessage()
		wsObservation("ws.text", target, string(data), nil, err)
	}
	binary := []byte{0x00, 0x01, 0xff, 0xfe}
	if err := conn.WriteMessage(websocket.BinaryMessage, binary); err != nil {
		wsObservation("ws.binary", target, "", nil, err)
	} else {
		_, data, err := conn.ReadMessage()
		wsObservation("ws.binary", target, "", data, err)
	}
	large := bytes.Repeat([]byte("x"), 64*1024)
	if err := conn.WriteMessage(websocket.TextMessage, large); err != nil {
		wsObservation("ws.large", target, "", nil, err)
	} else {
		_, data, err := conn.ReadMessage()
		wsObservation("ws.large", target, string(data), nil, err)
	}
}

// ---------------------------------------------------------------------------
// raw HTTP/1.x cases
// ---------------------------------------------------------------------------

type rawCase struct {
	ID      string
	Request string
}

func rawCases() []rawCase {
	big := strings.Repeat("a", 70*1024)
	return []rawCase{
		{ID: "raw.get_plain", Request: "GET /plain HTTP/1.1\r\nHost: x\r\n\r\n"},
		{ID: "raw.h1_0", Request: "GET /plain HTTP/1.0\r\n\r\n"},
		{ID: "raw.cl_te", Request: "POST /plain HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"},
		{ID: "raw.duplicate_cl", Request: "POST /plain HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\nbody"},
		{ID: "raw.bad_line", Request: "GARBAGE\r\n\r\n"},
		{ID: "raw.obs_fold", Request: "GET /plain HTTP/1.1\r\nHost: x\r\nX-Test: a\r\n b\r\n\r\n"},
		{ID: "raw.large_head", Request: "GET /plain HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n"},
	}
}

func rawObservation(target, caseID, base, request string) observation {
	obs := observation{Case: caseID, Target: target, Headers: map[string][]string{}, BodyEncoding: "utf8"}
	host := strings.TrimPrefix(strings.TrimPrefix(base, "http://"), "https://")
	conn, err := net.DialTimeout("tcp", host, 2*time.Second)
	if err != nil {
		reason := "connect"
		obs.Error = &reason
		return obs
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	// An oversized head may be rejected before the whole write completes; the
	// response is still on the socket, so ignore write errors and read.
	_, _ = conn.Write([]byte(request))
	buffer := make([]byte, 8192)
	n, _ := conn.Read(buffer)
	if n == 0 {
		reason := "other"
		obs.Error = &reason
		return obs
	}
	text := string(buffer[:n])
	line := text
	if index := strings.Index(text, "\r\n"); index >= 0 {
		line = text[:index]
	}
	fields := strings.Fields(line)
	if len(fields) < 2 {
		reason := "other"
		obs.Error = &reason
		return obs
	}
	code, err := strconv.Atoi(fields[1])
	if err != nil {
		reason := "other"
		obs.Error = &reason
		return obs
	}
	obs.Status = &code
	obs.Version = fields[0]
	return obs
}

func runRawGroup(h harness) {
	for _, raw := range rawCases() {
		for _, target := range []string{"cpp", "go"} {
			emit(rawObservation(target, raw.ID, h.rawBase(target), raw.Request))
		}
	}
}

// ---------------------------------------------------------------------------
// stateful flows
// ---------------------------------------------------------------------------

func runSession(h harness, target string) {
	base := h.base(target, caseSpec{})
	jar, _ := cookiejar.New(nil)
	client := clientFor("h1", jar)
	set, _ := http.NewRequest("POST", base+"/session/set", nil)
	resp, err := client.Do(set)
	if err == nil {
		_ = resp.Body.Close()
	}
	get, _ := http.NewRequest("GET", base+"/session/get", nil)
	resp, err = client.Do(get)
	obs := observe(resp, err, false)
	obs.Case = "framework.session"
	obs.Target = target
	emit(obs)
}

func runCSRF(h harness, target string) {
	base := h.base(target, caseSpec{})
	jar, _ := cookiejar.New(nil)
	client := clientFor("h1", jar)
	get, _ := http.NewRequest("GET", base+"/csrf/token", nil)
	resp, err := client.Do(get)
	token := ""
	if err == nil {
		data, _ := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		token = string(data)
	}
	post, _ := http.NewRequest("POST", base+"/csrf/submit", nil)
	post.Header.Set("X-Csrf-Token", token)
	resp, err = client.Do(post)
	obs := observe(resp, err, false)
	obs.Case = "framework.csrf_valid"
	obs.Target = target
	emit(obs)

	post, _ = http.NewRequest("POST", base+"/csrf/submit", nil)
	resp, err = client.Do(post)
	obs = observe(resp, err, false)
	obs.Case = "framework.csrf_missing"
	obs.Target = target
	emit(obs)
}

func mintHS256(header, payload, secret string) string {
	encodedHeader := base64.RawURLEncoding.EncodeToString([]byte(header))
	encodedPayload := base64.RawURLEncoding.EncodeToString([]byte(payload))
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(encodedHeader + "." + encodedPayload))
	return encodedHeader + "." + encodedPayload + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func jwtClaims(iss, aud string, exp, nbf int64) string {
	return fmt.Sprintf(`{"sub":"alice","iss":%q,"aud":%q,"exp":%d,"nbf":%d}`, iss, aud, exp, nbf)
}

func runJWT(h harness, target string) {
	base := h.base(target, caseSpec{})
	now := time.Now()
	const header = `{"alg":"HS256","typ":"JWT"}`
	validClaims := jwtClaims("iss", "aud", now.Add(time.Hour).Unix(), 0)
	valid := mintHS256(header, validClaims, "secret")

	emitAuth := func(caseName, authorization string) {
		request, _ := http.NewRequest("GET", base+"/jwt/me", nil)
		if authorization != "" {
			request.Header.Set("Authorization", authorization)
		}
		resp, err := clientFor("h1", nil).Do(request)
		obs := observe(resp, err, false)
		obs.Case = caseName
		obs.Target = target
		emit(obs)
	}

	emitAuth("framework.jwt_ok", "Bearer "+valid)
	emitAuth("framework.jwt_missing", "")
	emitAuth("framework.jwt_expired",
		"Bearer "+mintHS256(header, jwtClaims("iss", "aud", now.Add(-time.Hour).Unix(), 0), "secret"))
	emitAuth("framework.jwt_not_yet_valid",
		"Bearer "+mintHS256(header, jwtClaims("iss", "aud", now.Add(time.Hour).Unix(), now.Add(time.Hour).Unix()), "secret"))
	emitAuth("framework.jwt_wrong_issuer",
		"Bearer "+mintHS256(header, jwtClaims("other", "aud", now.Add(time.Hour).Unix(), 0), "secret"))
	emitAuth("framework.jwt_wrong_audience",
		"Bearer "+mintHS256(header, jwtClaims("iss", "other", now.Add(time.Hour).Unix(), 0), "secret"))
	emitAuth("framework.jwt_tampered", "Bearer "+mintHS256(header, validClaims, "wrong-secret"))
	// alg:none with an empty signature must be rejected by the allowlist.
	noneToken := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none"}`)) + "." +
		base64.RawURLEncoding.EncodeToString([]byte(validClaims)) + "."
	emitAuth("framework.jwt_alg_none", "Bearer "+noneToken)
	emitAuth("framework.jwt_malformed", "Bearer not-a-jwt")
	emitAuth("framework.jwt_bad_scheme", "Basic abc")
}

func runETag(h harness, target string) {
	base := h.base(target, caseSpec{})
	client := clientFor("h1", nil)
	first, _ := http.NewRequest("GET", base+"/data.txt", nil)
	resp, err := client.Do(first)
	etag := ""
	if err == nil {
		etag = resp.Header.Get("ETag")
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
	}
	second, _ := http.NewRequest("GET", base+"/data.txt", nil)
	if etag != "" {
		second.Header.Set("If-None-Match", etag)
	}
	resp, err = client.Do(second)
	obs := observe(resp, err, false)
	obs.Case = "static.etag_304"
	obs.Target = target
	if obs.Notes == nil {
		obs.Notes = map[string]string{}
	}
	obs.Notes["etag_present"] = fmt.Sprint(etag != "")
	emit(obs)
}

func runRate(h harness, target string) {
	base := h.base(target, caseSpec{})
	client := clientFor("h1", nil)
	statuses := []string{}
	retryAfter := ""
	for i := 0; i < 2; i++ {
		request, _ := http.NewRequest("GET", base+"/rate", nil)
		resp, err := client.Do(request)
		if err != nil {
			statuses = append(statuses, "error")
			continue
		}
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
		statuses = append(statuses, fmt.Sprint(resp.StatusCode))
		if value := resp.Header.Get("Retry-After"); value != "" {
			retryAfter = value
		}
	}
	obs := observation{Case: "middleware.rate", Target: target,
		Headers: map[string][]string{}, Body: strings.Join(statuses, ","), BodyEncoding: "utf8"}
	if retryAfter != "" {
		obs.Notes = map[string]string{"retry-after": retryAfter}
	}
	emit(obs)
}

func runKeepAlive(h harness, target string) {
	base := h.base(target, caseSpec{})
	client := clientFor("h1", nil)
	var statuses []string
	for i := 0; i < 5; i++ {
		request, _ := http.NewRequest("GET", base+"/plain", nil)
		resp, err := client.Do(request)
		if err != nil {
			statuses = append(statuses, "error")
			continue
		}
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
		statuses = append(statuses, fmt.Sprint(resp.StatusCode))
	}
	obs := observation{Case: "semantics.keep_alive", Target: target,
		Headers: map[string][]string{}, Body: strings.Join(statuses, ","), BodyEncoding: "utf8"}
	emit(obs)
}

func main() {
	group := flag.String("group", defaultGroup, "group: routing|body|semantics|middleware|framework|static|ws")
	cppH1 := flag.String("cpp-h1", "http://127.0.0.1:7820", "C++ h1 base")
	cppH2C := flag.String("cpp-h2c", "http://127.0.0.1:7821", "C++ h2c base")
	cppTLS := flag.String("cpp-tls", "https://127.0.0.1:7822", "C++ TLS base")
	cppH3 := flag.String("cpp-h3", "https://127.0.0.1:7823", "C++ h3 base")
	goH1 := flag.String("go-h1", "http://127.0.0.1:7830", "Go h1 base")
	goH2C := flag.String("go-h2c", "http://127.0.0.1:7831", "Go h2c base")
	goTLS := flag.String("go-tls", "https://127.0.0.1:7832", "Go TLS base")
	goH3 := flag.String("go-h3", "https://127.0.0.1:7833", "Go h3 base")
	cppMTLS := flag.String("cpp-mtls", "https://127.0.0.1:7825", "C++ mutual-TLS base")
	goMTLS := flag.String("go-mtls", "https://127.0.0.1:7834", "Go mutual-TLS base")
	cppRaw := flag.String("cpp-raw", "http://127.0.0.1:7824", "C++ raw h1 base")
	goRaw := flag.String("go-raw", "http://127.0.0.1:7830", "Go raw h1 base")
	flag.Parse()

	h := harness{cppH1: *cppH1, cppH2C: *cppH2C, cppTLS: *cppTLS, cppH3: *cppH3, cppMTLS: *cppMTLS,
		goH1: *goH1, goH2C: *goH2C, goTLS: *goTLS, goH3: *goH3, goMTLS: *goMTLS,
		cppRaw: *cppRaw, goRaw: *goRaw}

	var cases []caseSpec
	switch *group {
	case "routing":
		cases = routingCases()
	case "body":
		cases = bodyCases()
	case "semantics":
		cases = semanticsCases()
	case "middleware":
		cases = middlewareCases()
	case "framework":
		cases = frameworkCases()
	case "static":
		cases = staticCases()
	case "ws":
		for _, target := range []string{"cpp", "go"} {
			base := h.base(target, caseSpec{})
			runWS(target, base)
			runWSOrigin(target, base)
		}
		return
	case "raw":
		runRawGroup(h)
		return
	default:
		fmt.Fprintf(os.Stderr, "unknown group %q\n", *group)
		os.Exit(2)
	}

	for _, spec := range cases {
		h.run(spec)
	}

	switch *group {
	case "semantics":
		for _, target := range []string{"cpp", "go"} {
			runKeepAlive(h, target)
		}
	case "middleware":
		for _, target := range []string{"cpp", "go"} {
			runRate(h, target)
		}
	case "framework":
		for _, target := range []string{"cpp", "go"} {
			runSession(h, target)
			runCSRF(h, target)
			runJWT(h, target)
		}
	case "static":
		for _, target := range []string{"cpp", "go"} {
			runETag(h, target)
		}
	}
}
