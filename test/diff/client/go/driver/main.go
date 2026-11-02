// Go client driver for the client differential.
//
// It reads the same JSON case files as the C++ driver and prints one JSON
// observation per case. The two outputs are compared line by line; any
// difference is a client-behavior difference because both talked to the same
// reference server (refserver).
package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/cookiejar"
	"net/textproto"
	"net/url"
	"os"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/gorilla/websocket"
	"github.com/quic-go/quic-go/http3"
	"golang.org/x/net/http2"
)

// Replaced at build time with -ldflags "-X main.defaultGroup=body".
var defaultGroup = "basic"

type headerKV struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

type bodyPart struct {
	Name        string `json:"name"`
	Filename    string `json:"filename"`
	ContentType string `json:"content_type"`
	Data        string `json:"data"`
}

type bodySpec struct {
	Kind  string     `json:"kind"`
	Value string     `json:"value"`
	Parts []bodyPart `json:"parts"`
}

type retrySpec struct {
	MaxRetries    int   `json:"max_retries"`
	RetryStatuses []int `json:"retry_statuses"`
}

type clientSpec struct {
	Version          string     `json:"version"`
	MaxRedirects     int        `json:"max_redirects"`
	AutoDecompress   *bool      `json:"auto_decompress"`
	AcceptEncodings  []string   `json:"accept_encodings"`
	RequestTimeoutMS int        `json:"request_timeout_ms"`
	MaxBodyBytes     int64      `json:"max_body_bytes"`
	CookieJar        bool       `json:"cookie_jar"`
	Retry            *retrySpec `json:"retry"`
	TLSVerify        *bool      `json:"tls_verify"`
}

type requestSpec struct {
	Method  string     `json:"method"`
	Path    string     `json:"path"`
	Headers []headerKV `json:"headers"`
	Query   []headerKV `json:"query"`
	Body    *bodySpec  `json:"body"`
}

type expectSpec struct {
	Status         int      `json:"status"`
	StatusIn       []int    `json:"status_in"`
	BodyMode       string   `json:"body_mode"`
	CompareHeaders []string `json:"compare_headers"`
	IgnoreHeaders  []string `json:"ignore_headers"`
	CompareVersion bool     `json:"compare_version"`
	CompareFraming bool     `json:"compare_framing"`
}

type testCase struct {
	ID      string      `json:"id"`
	Mode    string      `json:"mode"`
	Client  clientSpec  `json:"client"`
	Request requestSpec `json:"request"`
	Expect  expectSpec  `json:"expect"`
}

type observation struct {
	Case         string              `json:"case"`
	Target       string              `json:"target,omitempty"`
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
		fmt.Fprintf(os.Stderr, "marshal observation: %v\n", err)
		return
	}
	fmt.Println(string(data))
}

func boolValue(value *bool, fallback bool) bool {
	if value == nil {
		return fallback
	}
	return *value
}

func baseFor(version string) (string, http.RoundTripper, func()) {
	noop := func() {}
	switch version {
	case "h2":
		transport := &http.Transport{
			TLSClientConfig:   &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12},
			ForceAttemptHTTP2: true,
		}
		return "https://127.0.0.1:7811", transport, transport.CloseIdleConnections
	case "h2c":
		transport := &http2.Transport{
			AllowHTTP: true,
			DialTLSContext: func(ctx context.Context, network, addr string,
				cfg *tls.Config) (net.Conn, error) {
				var dialer net.Dialer
				return dialer.DialContext(ctx, network, addr)
			},
		}
		return "http://127.0.0.1:7812", transport, noop
	case "h3":
		transport := &http3.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13},
		}
		return "https://127.0.0.1:7813", transport, func() { _ = transport.Close() }
	default:
		transport := http.DefaultTransport.(*http.Transport).Clone()
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12}
		transport.ForceAttemptHTTP2 = false
		return "http://127.0.0.1:7810", transport, transport.CloseIdleConnections
	}
}

func newClient(spec clientSpec) (*http.Client, string, func()) {
	base, transport, cleanup := baseFor(spec.Version)
	if baseConfig, ok := transport.(*http.Transport); ok {
		baseConfig.DisableCompression = !boolValue(spec.AutoDecompress, true)
	}
	timeout := time.Duration(spec.RequestTimeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	client := &http.Client{Transport: transport, Timeout: timeout}
	if spec.MaxRedirects <= 0 {
		client.CheckRedirect = func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		}
	} else {
		limit := spec.MaxRedirects
		client.CheckRedirect = func(_ *http.Request, via []*http.Request) error {
			if len(via) > limit {
				return errors.New("too many redirects")
			}
			return nil
		}
	}
	if spec.CookieJar {
		jar, _ := cookiejar.New(nil)
		client.Jar = jar
	}
	return client, base, cleanup
}

func addQuery(path string, pairs []headerKV) string {
	if len(pairs) == 0 {
		return path
	}
	var builder strings.Builder
	builder.WriteString(path)
	if strings.Contains(path, "?") {
		builder.WriteString("&")
	} else {
		builder.WriteString("?")
	}
	for index, pair := range pairs {
		if index > 0 {
			builder.WriteString("&")
		}
		builder.WriteString(url.QueryEscape(pair.Name))
		builder.WriteString("=")
		builder.WriteString(url.QueryEscape(pair.Value))
	}
	return builder.String()
}

func buildMultipart(spec *bodySpec) ([]byte, string, error) {
	var buffer bytes.Buffer
	writer := multipart.NewWriter(&buffer)
	if err := writer.SetBoundary("DIFFBOUNDARY"); err != nil {
		return nil, "", err
	}
	for _, part := range spec.Parts {
		if part.Filename == "" {
			_ = writer.WriteField(part.Name, part.Data)
			continue
		}
		header := make(textproto.MIMEHeader)
		header.Set("Content-Disposition",
			fmt.Sprintf(`form-data; name=%q; filename=%q`, part.Name, part.Filename))
		if part.ContentType != "" {
			header.Set("Content-Type", part.ContentType)
		}
		item, err := writer.CreatePart(header)
		if err != nil {
			return nil, "", err
		}
		_, _ = item.Write([]byte(part.Data))
	}
	if err := writer.Close(); err != nil {
		return nil, "", err
	}
	return buffer.Bytes(), writer.FormDataContentType(), nil
}

func buildRequest(c testCase, base string) (*http.Request, error) {
	target := c.Request.Path
	if target == "" {
		target = "/"
	}
	target = addQuery(target, c.Request.Query)
	method := c.Request.Method
	if method == "" {
		method = http.MethodGet
	}
	var body io.Reader
	contentType := ""
	if c.Request.Body != nil {
		switch c.Request.Body.Kind {
		case "", "none":
		case "text", "json":
			body = strings.NewReader(c.Request.Body.Value)
			if c.Request.Body.Kind == "json" {
				contentType = "application/json"
			}
		case "multipart":
			data, formType, err := buildMultipart(c.Request.Body)
			if err != nil {
				return nil, err
			}
			body = bytes.NewReader(data)
			contentType = formType
		default:
			return nil, fmt.Errorf("unknown body kind %q", c.Request.Body.Kind)
		}
	}
	request, err := http.NewRequest(method, base+target, body)
	if err != nil {
		return nil, err
	}
	for _, header := range c.Request.Headers {
		request.Header.Add(header.Name, header.Value)
	}
	if contentType != "" && request.Header.Get("Content-Type") == "" {
		request.Header.Set("Content-Type", contentType)
	}
	if request.Header.Get("User-Agent") == "" {
		request.Header.Set("User-Agent", "diff-probe")
	}
	return request, nil
}

func classifyError(err error, bodyTooLarge bool) string {
	if bodyTooLarge {
		return "body_too_large"
	}
	if err == nil {
		return ""
	}
	if errors.Is(err, context.DeadlineExceeded) || os.IsTimeout(err) {
		return "timeout"
	}
	var netError net.Error
	if errors.As(err, &netError) && netError.Timeout() {
		return "timeout"
	}
	text := err.Error()
	if strings.Contains(text, "connection refused") || strings.Contains(text, "connect:") {
		return "connect"
	}
	if strings.Contains(text, "too many redirects") {
		return "too_many_redirects"
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

func observeResponse(resp *http.Response, err error, maxBody int64) observation {
	obs := observation{Target: "go"}
	if err != nil {
		reason := classifyError(err, false)
		obs.Error = &reason
		return obs
	}
	defer resp.Body.Close()
	status := resp.StatusCode
	obs.Status = &status
	obs.Version = resp.Proto
	obs.Framing = framingOf(resp)
	obs.Headers = map[string][]string{}
	for name, values := range resp.Header {
		obs.Headers[strings.ToLower(name)] = values
	}
	reader := io.Reader(resp.Body)
	if maxBody > 0 {
		reader = io.LimitReader(resp.Body, maxBody+1)
	}
	data, readErr := io.ReadAll(reader)
	if maxBody > 0 && int64(len(data)) > maxBody {
		reason := "body_too_large"
		obs.Error = &reason
		return obs
	}
	if readErr != nil {
		reason := classifyError(readErr, false)
		obs.Error = &reason
		return obs
	}
	if utf8.Valid(data) {
		obs.Body = string(data)
		obs.BodyEncoding = "utf8"
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

func runDeclarative(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var cases []testCase
	if err := json.Unmarshal(data, &cases); err != nil {
		return err
	}
	for _, c := range cases {
		if c.Mode == "cpp-only" {
			continue
		}
		obs := runOne(c)
		obs.Case = c.ID
		emit(obs)
	}
	return nil
}

func runOne(c testCase) observation {
	client, base, cleanup := newClient(c.Client)
	defer cleanup()
	request, err := buildRequest(c, base)
	if err != nil {
		reason := "other"
		return observation{Error: &reason}
	}
	resp, err := client.Do(request)
	return observeResponse(resp, err, c.Client.MaxBodyBytes)
}

// --- streaming body group ---------------------------------------------------

func runBodyGroup() error {
	base, _, cleanup := baseFor("h1")
	defer cleanup()

	// 1. Streaming multipart: field + in-memory file part.
	{
		pipeReader, pipeWriter := io.Pipe()
		writer := multipart.NewWriter(pipeWriter)
		_ = writer.SetBoundary("DIFFBOUNDARY")
		go func() {
			_ = writer.WriteField("title", "hello")
			header := make(textproto.MIMEHeader)
			header.Set("Content-Disposition", `form-data; name="upload"; filename="notes.txt"`)
			header.Set("Content-Type", "text/plain")
			part, err := writer.CreatePart(header)
			if err == nil {
				_, _ = part.Write([]byte("file data"))
			}
			_ = writer.Close()
			_ = pipeWriter.Close()
		}()
		request, _ := http.NewRequest(http.MethodPost, base+"/multipart", pipeReader)
		request.Header.Set("Content-Type", writer.FormDataContentType())
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.streaming_multipart"
		emit(obs)
	}

	// 2. Raw file-body upload (200000 deterministic bytes).
	{
		payload := make([]byte, 200000)
		for index := range payload {
			payload[index] = byte('a' + index%26)
		}
		request, _ := http.NewRequest(http.MethodPut, base+"/echo", bytes.NewReader(payload))
		// Force chunked framing: the C++ probe streams the file and cannot know
		// its length up front. The /echo response includes the request headers,
		// so matching framing is part of matching behavior.
		request.ContentLength = -1
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.file_upload"
		emit(obs)
	}

	// 3. Expect: 100-continue accepted.
	{
		request, _ := http.NewRequest(http.MethodPost, base+"/expect-ok", strings.NewReader("hello!"))
		request.Header.Set("Expect", "100-continue")
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.expect_ok"
		emit(obs)
	}

	// 4. Expect rejected with a final response.
	{
		request, _ := http.NewRequest(http.MethodPost, base+"/expect-reject", strings.NewReader("body"))
		request.Header.Set("Expect", "100-continue")
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.expect_reject"
		emit(obs)
	}

	// 5. Request trailers.
	{
		request, _ := http.NewRequest(http.MethodPost, base+"/request-trailer",
			strings.NewReader("body"))
		request.ContentLength = -1
		request.Trailer = http.Header{"X-Check": []string{"yes"}}
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.request_trailers"
		emit(obs)
	}

	// 6. Response trailers.
	{
		request, _ := http.NewRequest(http.MethodGet, base+"/trailers", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "body.response_trailers"
		emit(obs)
	}

	// 7. SSE.
	{
		request, _ := http.NewRequest(http.MethodGet, base+"/sse", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observation{Case: "body.sse", Target: "go"}
		if err != nil {
			reason := classifyError(err, false)
			obs.Error = &reason
			emit(obs)
			return nil
		}
		var events []string
		scanner := bufio.NewScanner(resp.Body)
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "data: ") {
				events = append(events, strings.TrimPrefix(line, "data: "))
			}
		}
		_ = resp.Body.Close()
		obs.Body = strings.Join(events, "|")
		obs.BodyEncoding = "utf8"
		status := resp.StatusCode
		obs.Status = &status
		emit(obs)
	}

	// 8. max_body_bytes.
	{
		request, _ := http.NewRequest(http.MethodGet, base+"/large?n=100000", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := (&http.Client{Timeout: 10 * time.Second}).Do(request)
		obs := observeResponse(resp, err, 1024)
		obs.Case = "body.max_body_bytes"
		emit(obs)
	}
	return nil
}

// --- policy group -----------------------------------------------------------

func runPolicyGroup() error {
	base, _, cleanup := baseFor("h1")
	defer cleanup()

	// CookieJar across requests.
	{
		jar, _ := cookiejar.New(nil)
		client := &http.Client{Jar: jar, Timeout: 10 * time.Second}
		request, _ := http.NewRequest(http.MethodGet, base+"/cookies/set", nil)
		request.Header.Set("User-Agent", "diff-probe")
		_, _ = client.Do(request)
		request, _ = http.NewRequest(http.MethodGet, base+"/cookies", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := client.Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "policy.cookies"
		emit(obs)
	}

	// Request timeout.
	{
		client := &http.Client{Timeout: 200 * time.Millisecond}
		request, _ := http.NewRequest(http.MethodGet, base+"/slow?ms=2000", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := client.Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "policy.timeout"
		emit(obs)
	}

	// Default status handling: no retry, the 429 is returned to the caller.
	{
		reset, _ := http.NewRequest(http.MethodGet, base+"/retry/reset", nil)
		_, _ = (&http.Client{Timeout: 5 * time.Second}).Do(reset)
		client := &http.Client{Timeout: 5 * time.Second}
		request, _ := http.NewRequest(http.MethodGet, base+"/retry/2", nil)
		request.Header.Set("User-Agent", "diff-probe")
		resp, err := client.Do(request)
		obs := observeResponse(resp, err, 0)
		obs.Case = "policy.retry_default_off"
		emit(obs)
	}
	return nil
}

// --- WebSocket group --------------------------------------------------------

func wsObservation(caseName, text string, data []byte, err error) {
	obs := observation{Case: caseName, Target: "go"}
	if err != nil {
		reason := "other"
		obs.Error = &reason
		emit(obs)
		return
	}
	if text != "" {
		obs.Body = text
		obs.BodyEncoding = "utf8"
	} else if utf8.Valid(data) {
		obs.Body = string(data)
		obs.BodyEncoding = "utf8"
	} else {
		obs.Body = base64.StdEncoding.EncodeToString(data)
		obs.BodyEncoding = "base64"
	}
	emit(obs)
}

func runWSGroup() error {
	conn, _, err := websocket.DefaultDialer.Dial("ws://127.0.0.1:7810/ws", nil)
	if err != nil {
		wsObservation("ws.text", "", nil, err)
		wsObservation("ws.binary", "", nil, err)
		wsObservation("ws.large", "", nil, err)
		return nil
	}
	defer conn.Close()

	// text
	if err := conn.WriteMessage(websocket.TextMessage, []byte("hello")); err != nil {
		wsObservation("ws.text", "", nil, err)
	} else {
		_, data, readErr := conn.ReadMessage()
		wsObservation("ws.text", string(data), nil, readErr)
	}
	// binary
	binary := []byte{0x00, 0x01, 0xff, 0xfe}
	if err := conn.WriteMessage(websocket.BinaryMessage, binary); err != nil {
		wsObservation("ws.binary", "", nil, err)
	} else {
		_, data, readErr := conn.ReadMessage()
		wsObservation("ws.binary", "", data, readErr)
	}
	// large
	large := bytes.Repeat([]byte("x"), 64*1024)
	if err := conn.WriteMessage(websocket.TextMessage, large); err != nil {
		wsObservation("ws.large", "", nil, err)
	} else {
		_, data, readErr := conn.ReadMessage()
		wsObservation("ws.large", string(data), nil, readErr)
	}
	return nil
}

func main() {
	group := flag.String("group", defaultGroup, "case group: basic|body|policy|ws|h3")
	casesFile := flag.String("cases", "", "JSON case file (basic/policy/h3)")
	flag.Parse()

	var err error
	switch *group {
	case "basic", "h3":
		if *casesFile == "" {
			err = fmt.Errorf("--cases is required for group %s", *group)
			break
		}
		err = runDeclarative(*casesFile)
	case "policy":
		if *casesFile != "" {
			if err = runDeclarative(*casesFile); err != nil {
				break
			}
		}
		err = runPolicyGroup()
	case "body":
		err = runBodyGroup()
	case "ws":
		err = runWSGroup()
	default:
		err = fmt.Errorf("unknown group %q", *group)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
