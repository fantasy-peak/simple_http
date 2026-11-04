// Go reference server for the server differential.
//
// It implements the same observable route table as
// test/diff/server/cpp/server.cpp using only the Go standard library (plus
// gorilla/websocket and quic-go for the /ws and h3 listeners). The Go probes
// send identical requests to both servers; the runner compares the responses.
//
//	7830  plaintext h1
//	7831  h2c
//	7832  TLS h2 + http/1.1
//	7833  TCP + UDP HTTP/3
package main

import (
	"bytes"
	"compress/gzip"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"mime"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/signal"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/quic-go/quic-go/http3"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

// ---------------------------------------------------------------------------
// shared types and helpers
// ---------------------------------------------------------------------------

type createUser struct {
	Name string `json:"name"`
	Age  int    `json:"age"`
}

type echoResponse struct {
	Method   string              `json:"method"`
	Path     string              `json:"path"`
	RawQuery string              `json:"raw_query"`
	Proto    string              `json:"proto"`
	Host     string              `json:"host"`
	Headers  map[string][]string `json:"headers"`
	Trailers map[string][]string `json:"trailers"`
	BodyB64  string              `json:"body_b64"`
}

type multipartPart struct {
	Name        string `json:"name"`
	Filename    string `json:"filename"`
	ContentType string `json:"content_type"`
	Size        int    `json:"size"`
	BodyB64     string `json:"body_b64"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Details any    `json:"details,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	data, err := json.Marshal(value)
	if err != nil {
		data = []byte("{}")
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(data)
}

func writeText(w http.ResponseWriter, status int, body string) {
	if status != http.StatusNoContent && status != http.StatusNotModified {
		w.Header().Set("Content-Type", "text/plain")
	}
	w.WriteHeader(status)
	if status != http.StatusNoContent && status != http.StatusNotModified {
		_, _ = io.WriteString(w, body)
	}
}

func writeError(w http.ResponseWriter, status int, code, message string, details any) {
	writeJSON(w, status, errorBody{Code: code, Message: message, Details: details})
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

func readAll(r *http.Request) []byte {
	data, _ := io.ReadAll(r.Body)
	return data
}

// ---------------------------------------------------------------------------
// server state
// ---------------------------------------------------------------------------

type serverState struct {
	mu       sync.Mutex
	sessions map[string]map[string]string
	rateHits int
}

func newState() *serverState {
	return &serverState{sessions: map[string]map[string]string{}}
}

func randomToken(size int) string {
	buffer := make([]byte, size)
	if _, err := rand.Read(buffer); err != nil {
		panic(err)
	}
	return base64.RawURLEncoding.EncodeToString(buffer)
}

// ---------------------------------------------------------------------------
// routes
// ---------------------------------------------------------------------------

func echoHandler(w http.ResponseWriter, r *http.Request) {
	body := readAll(r)
	headers := map[string][]string{}
	hidden := map[string]bool{
		"host": true, "connection": true, "keep-alive": true,
		"transfer-encoding": true, "content-length": true, "upgrade": true,
	}
	for name, values := range r.Header {
		lower := strings.ToLower(name)
		if hidden[lower] {
			continue
		}
		headers[lower] = values
	}
	trailers := map[string][]string{}
	for name, values := range r.Trailer {
		trailers[strings.ToLower(name)] = values
	}
	writeJSON(w, http.StatusOK, echoResponse{
		Method: r.Method, Path: r.URL.Path, RawQuery: r.URL.RawQuery,
		Proto: protoName(r.Proto), Host: r.Host, Headers: headers,
		Trailers: trailers, BodyB64: base64.StdEncoding.EncodeToString(body),
	})
}

func multipartHandler(w http.ResponseWriter, r *http.Request) {
	reader, err := r.MultipartReader()
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "not multipart"})
		return
	}
	parts := []multipartPart{}
	for {
		item, err := reader.NextPart()
		if err == io.EOF {
			break
		}
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad multipart"})
			return
		}
		data, _ := io.ReadAll(item)
		parts = append(parts, multipartPart{
			Name: item.FormName(), Filename: item.FileName(),
			ContentType: item.Header.Get("Content-Type"), Size: len(data),
			BodyB64: base64.StdEncoding.EncodeToString(data),
		})
		_ = item.Close()
	}
	writeJSON(w, http.StatusOK, parts)
}

func whoHandler(w http.ResponseWriter, r *http.Request) {
	host := strings.ToLower(strings.TrimRight(r.Host, "."))
	withoutPort := host
	if strings.Count(host, ":") == 1 {
		withoutPort = host[:strings.Index(host, ":")]
	}
	switch {
	case host == "api.example.com:8080":
		writeText(w, http.StatusOK, "port")
	case withoutPort == "api.example.com":
		writeText(w, http.StatusOK, "api")
	case strings.HasSuffix(withoutPort, ".example.com"):
		writeText(w, http.StatusOK, "sub")
	default:
		writeText(w, http.StatusOK, "root")
	}
}

func allowHeader(methods string) string {
	parts := strings.Split(methods, ", ")
	out := make([]string, 0, len(parts)+2)
	for _, method := range parts {
		out = append(out, method)
		if method == http.MethodGet {
			out = append(out, http.MethodHead)
		}
	}
	out = append(out, http.MethodOptions)
	return strings.Join(out, ", ")
}

func allow(w http.ResponseWriter, r *http.Request, methods string) bool {
	if r.Method == http.MethodOptions {
		w.Header().Set("Allow", allowHeader(methods))
		w.WriteHeader(http.StatusNoContent)
		return false
	}
	for _, method := range strings.Split(methods, ", ") {
		if r.Method == method || (method == http.MethodGet && r.Method == http.MethodHead) {
			return true
		}
	}
	w.Header().Set("Allow", allowHeader(methods))
	// The library's built-in 405 spells the type with a charset; keep the two
	// reference servers byte-comparable.
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusMethodNotAllowed)
	_, _ = io.WriteString(w, "method not allowed")
	return false
}

func csrfToken(w http.ResponseWriter, r *http.Request) string {
	if cookie, err := r.Cookie("csrf"); err == nil && cookie.Value != "" {
		return cookie.Value
	}
	token := randomToken(32)
	http.SetCookie(w, &http.Cookie{Name: "csrf", Value: token, Path: "/", SameSite: http.SameSiteLaxMode})
	return token
}

func loadSession(state *serverState, w http.ResponseWriter, r *http.Request) string {
	id := ""
	if cookie, err := r.Cookie("sid"); err == nil && cookie.Value != "" {
		id = cookie.Value
	} else {
		id = randomToken(32)
	}
	http.SetCookie(w, &http.Cookie{
		Name: "sid", Value: id, Path: "/", HttpOnly: true,
		SameSite: http.SameSiteLaxMode, MaxAge: 86400,
	})
	return id
}

func verifyJWT(token string) (string, bool) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return "", false
	}
	headerData, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return "", false
	}
	var header struct {
		Alg string `json:"alg"`
	}
	if json.Unmarshal(headerData, &header) != nil || header.Alg != "HS256" {
		return "", false
	}
	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return "", false
	}
	mac := hmac.New(sha256.New, []byte("secret"))
	_, _ = mac.Write([]byte(parts[0] + "." + parts[1]))
	if subtle.ConstantTimeCompare(mac.Sum(nil), signature) != 1 {
		return "", false
	}
	payloadData, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return "", false
	}
	var payload struct {
		Sub string `json:"sub"`
		Iss string `json:"iss"`
		Aud string `json:"aud"`
		Exp int64  `json:"exp"`
		Nbf int64  `json:"nbf"`
	}
	if json.Unmarshal(payloadData, &payload) != nil {
		return "", false
	}
	now := time.Now().Unix()
	if payload.Exp == 0 || now > payload.Exp+60 {
		return "", false
	}
	if payload.Nbf != 0 && now+60 < payload.Nbf {
		return "", false
	}
	if payload.Iss != "iss" || payload.Aud != "aud" {
		return "", false
	}
	return payload.Sub, true
}

func jwtHandler(w http.ResponseWriter, r *http.Request) {
	auth := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if auth == "" {
		w.Header().Set("WWW-Authenticate", `Bearer error="invalid_token"`)
		writeError(w, http.StatusUnauthorized, "unauthorized", "missing bearer token", nil)
		return
	}
	if !strings.HasPrefix(auth, prefix) {
		w.Header().Set("WWW-Authenticate", `Bearer error="invalid_token"`)
		writeError(w, http.StatusUnauthorized, "unauthorized", "invalid authorization scheme", nil)
		return
	}
	subject, ok := verifyJWT(auth[len(prefix):])
	if !ok {
		w.Header().Set("WWW-Authenticate", `Bearer error="invalid_token"`)
		writeError(w, http.StatusUnauthorized, "unauthorized", "invalid bearer token", nil)
		return
	}
	writeText(w, http.StatusOK, subject)
}

func compressionHandler(w http.ResponseWriter, r *http.Request) {
	body := []byte(strings.Repeat("c", 2048))
	w.Header().Set("Content-Type", "text/plain")
	if !strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
		_, _ = w.Write(body)
		return
	}
	var buffer strings.Builder
	writer := gzip.NewWriter(&buffer)
	_, _ = writer.Write(body)
	_ = writer.Close()
	w.Header().Set("Content-Encoding", "gzip")
	w.Header().Set("Vary", "Accept-Encoding")
	_, _ = io.WriteString(w, buffer.String())
}

func corsHandler(w http.ResponseWriter, r *http.Request) {
	origin := r.Header.Get("Origin")
	allowed := false
	if strings.HasPrefix(origin, "https://") {
		host := strings.TrimPrefix(origin, "https://")
		allowed = strings.HasSuffix(host, ".example.com") || host == "example.com"
		if host == "example.com" {
			allowed = false
		}
	}
	if allowed {
		w.Header().Set("Access-Control-Allow-Origin", origin)
		w.Header().Set("Access-Control-Allow-Credentials", "true")
		w.Header().Add("Vary", "Origin")
	}
	if r.Method == http.MethodOptions {
		// Fetch caches a preflight per (origin, method, header list), so the
		// response must vary on all three inputs.
		w.Header().Add("Vary", "Access-Control-Request-Method")
		w.Header().Add("Vary", "Access-Control-Request-Headers")
		if allowed {
			if method := r.Header.Get("Access-Control-Request-Method"); method != "" {
				w.Header().Set("Access-Control-Allow-Methods", method)
			}
			if headers := r.Header.Get("Access-Control-Request-Headers"); headers != "" {
				w.Header().Set("Access-Control-Allow-Headers", headers)
			}
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeText(w, http.StatusOK, "cors")
}

func secureHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Security-Policy", "default-src 'self'")
	w.Header().Set("X-Frame-Options", "DENY")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Referrer-Policy", "strict-origin-when-cross-origin")
	if r.TLS != nil {
		w.Header().Set("Strict-Transport-Security", "max-age=60")
	}
	writeText(w, http.StatusOK, "secure")
}

func basicAuthHandler(w http.ResponseWriter, r *http.Request) {
	user, pass, ok := r.BasicAuth()
	if !ok || user != "user" || pass != "pass" {
		w.Header().Set("WWW-Authenticate", `Basic realm="restricted"`)
		writeText(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	writeText(w, http.StatusOK, "ok")
}

func staticHandler(root string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		requestPath := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
		if requestPath == "." || requestPath == "" {
			requestPath = "index.html"
		}
		full := filepath.Join(root, filepath.FromSlash(requestPath))
		if !strings.HasPrefix(full, filepath.Clean(root)+string(os.PathSeparator)) && full != filepath.Clean(root) {
			writeText(w, http.StatusNotFound, "not found")
			return
		}
		info, err := os.Stat(full)
		if err != nil || info.IsDir() {
			// SPA fallback: a missing extensionless path serves index.html.
			if !strings.Contains(path.Base(requestPath), ".") {
				full = filepath.Join(root, "index.html")
				info, err = os.Stat(full)
			}
		}
		if err != nil || info.IsDir() {
			writeText(w, http.StatusNotFound, "not found")
			return
		}
		contentType := mime.TypeByExtension(filepath.Ext(full))
		if contentType == "" {
			contentType = "application/octet-stream"
		}
		w.Header().Set("Content-Type", contentType)
		w.Header().Set("Vary", "Accept-Encoding")
		w.Header().Set("Accept-Ranges", "bytes")
		if path.Base(full) == "index.html" {
			w.Header().Set("Cache-Control", "no-cache")
		} else {
			w.Header().Set("Cache-Control", "max-age=86400, public")
		}
		setETag(w, info)
		// Precompressed siblings.
		if strings.Contains(r.Header.Get("Accept-Encoding"), "br") {
			if sibling := full + ".br"; fileExists(sibling) {
				w.Header().Set("Content-Encoding", "br")
				http.ServeFile(w, r, sibling)
				return
			}
		}
		if strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
			if sibling := full + ".gz"; fileExists(sibling) {
				w.Header().Set("Content-Encoding", "gzip")
				http.ServeFile(w, r, sibling)
				return
			}
		}
		http.ServeFile(w, r, full)
	}
}

func setETag(w http.ResponseWriter, info os.FileInfo) {
	w.Header().Set("Etag", fmt.Sprintf(`W/"%x-%x"`, info.ModTime().UnixNano(), info.Size()))
}

func fileExists(name string) bool {
	info, err := os.Stat(name)
	return err == nil && !info.IsDir()
}

func makeHandler(state *serverState, staticRoot string) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/plain", func(w http.ResponseWriter, r *http.Request) { writeText(w, http.StatusOK, "hello") })
	mux.HandleFunc("/echo", echoHandler)
	mux.HandleFunc("/multipart", multipartHandler)
	mux.HandleFunc("/client-cert", func(w http.ResponseWriter, r *http.Request) {
		cn := "none"
		if r.TLS != nil && len(r.TLS.PeerCertificates) > 0 {
			cn = r.TLS.PeerCertificates[0].Subject.CommonName
		}
		writeText(w, http.StatusOK, cn)
	})
	mux.HandleFunc("/status/", func(w http.ResponseWriter, r *http.Request) {
		code, err := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/status/"))
		if err != nil || code == 0 {
			code = 200
		}
		writeText(w, code, "status")
	})
	mux.HandleFunc("/redirect/", func(w http.ResponseWriter, r *http.Request) {
		code, err := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/redirect/"))
		if err != nil || code == 0 {
			code = 302
		}
		w.Header().Set("Content-Type", "text/plain")
		w.Header().Set("Location", "/echo")
		w.WriteHeader(code)
	})
	mux.HandleFunc("/files/", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		switch r.URL.Path {
		case "/files/static":
			writeText(w, http.StatusOK, "static")
		default:
			rest := strings.TrimPrefix(r.URL.Path, "/files/")
			if strings.Contains(rest, "/") {
				writeText(w, http.StatusOK, "rest="+rest)
			} else {
				writeText(w, http.StatusOK, "id="+rest)
			}
		}
	})
	mux.HandleFunc("/api/", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		writeText(w, http.StatusOK, "api path: "+r.URL.Path)
	})
	mux.HandleFunc("/method", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET, POST") {
			return
		}
		writeText(w, http.StatusOK, "method:"+r.Method)
	})
	mux.HandleFunc("/users/", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		id := strings.TrimPrefix(r.URL.Path, "/users/")
		value, err := strconv.ParseInt(id, 10, 64)
		if err != nil {
			writeError(w, 400, "invalid_path", "invalid or missing path parameters", nil)
			return
		}
		writeText(w, http.StatusOK, "id="+strconv.FormatInt(value, 10))
	})
	mux.HandleFunc("/state", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		writeText(w, http.StatusOK, "from-state")
	})
	mux.HandleFunc("/list", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		page, err := strconv.Atoi(r.URL.Query().Get("page"))
		if err != nil {
			writeError(w, 400, "invalid_query", "invalid or missing query parameters", nil)
			return
		}
		writeText(w, http.StatusOK, r.URL.Query().Get("q")+":"+strconv.Itoa(page))
	})
	mux.HandleFunc("/json", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "POST") {
			return
		}
		var in createUser
		if json.NewDecoder(r.Body).Decode(&in) != nil {
			writeError(w, 400, "invalid_json", "malformed JSON request body", "Invalid argument")
			return
		}
		writeText(w, http.StatusCreated, in.Name+":"+strconv.Itoa(in.Age))
	})
	mux.HandleFunc("/valid", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "POST") {
			return
		}
		var in createUser
		if json.NewDecoder(r.Body).Decode(&in) != nil {
			writeError(w, 400, "invalid_json", "malformed JSON request body", "Invalid argument")
			return
		}
		details := []map[string]string{}
		if in.Name == "" {
			details = append(details, map[string]string{"field": "name", "code": "required", "message": "required"})
		}
		if in.Age < 1 || in.Age > 150 {
			details = append(details, map[string]string{"field": "age", "code": "range", "message": "must be between the allowed bounds"})
		}
		if len(details) != 0 {
			writeError(w, 422, "validation_failed", "request validation failed", details)
			return
		}
		writeText(w, http.StatusCreated, in.Name)
	})
	mux.HandleFunc("/form", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "POST") {
			return
		}
		if err := r.ParseForm(); err != nil {
			writeText(w, http.StatusBadRequest, "invalid form")
			return
		}
		age, err := strconv.Atoi(r.PostForm.Get("age"))
		if err != nil {
			writeText(w, http.StatusBadRequest, "invalid form")
			return
		}
		writeText(w, http.StatusCreated, r.PostForm.Get("name")+":"+strconv.Itoa(age))
	})
	mux.HandleFunc("/header", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		rawPage := r.Header.Get("X-Page")
		if rawPage == "" {
			writeError(w, 400, "missing_header", "missing required header", "x-page")
			return
		}
		page, err := strconv.Atoi(rawPage)
		if err != nil {
			writeError(w, 400, "invalid_header", "invalid header value", "x-page")
			return
		}
		cookie, err := r.Cookie("sid")
		if err != nil {
			writeError(w, 400, "missing_cookie", "missing required cookie", "sid")
			return
		}
		writeText(w, http.StatusOK, strconv.Itoa(page)+":"+cookie.Value)
	})
	mux.HandleFunc("/backend", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, "backend")
	})
	proxyTarget, _ := url.Parse("http://127.0.0.1:7830")
	proxy := httputil.NewSingleHostReverseProxy(proxyTarget)
	mux.HandleFunc("/proxy", func(w http.ResponseWriter, r *http.Request) {
		r.URL.Path = "/backend"
		proxy.ServeHTTP(w, r)
	})
	mux.HandleFunc("/decode", func(w http.ResponseWriter, r *http.Request) {
		data := readAll(r)
		encoding := strings.ToLower(r.Header.Get("Content-Encoding"))
		switch encoding {
		case "":
			writeText(w, http.StatusOK, "ce=none;cl=none;"+string(data))
			return
		case "gzip":
		default:
			writeText(w, http.StatusUnsupportedMediaType, "unsupported content coding")
			return
		}
		reader, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			writeText(w, http.StatusBadRequest, "decode error")
			return
		}
		decoded, err := io.ReadAll(io.LimitReader(reader, (1<<20)+1))
		_ = reader.Close()
		if err != nil {
			writeText(w, http.StatusBadRequest, "decode error")
			return
		}
		if len(decoded) > 1<<20 {
			writeText(w, http.StatusRequestEntityTooLarge, "decode error")
			return
		}
		writeText(w, http.StatusOK, "ce=none;cl=none;"+string(decoded))
	})
	mux.HandleFunc("/delay", func(w http.ResponseWriter, r *http.Request) {
		ms, _ := strconv.Atoi(r.URL.Query().Get("ms"))
		if ms > 0 {
			time.Sleep(time.Duration(ms) * time.Millisecond)
		}
		writeText(w, http.StatusOK, "delayed")
	})
	mux.HandleFunc("/drain", func(w http.ResponseWriter, r *http.Request) {
		body := readAll(r)
		writeText(w, http.StatusOK, "received "+strconv.Itoa(len(body))+" bytes")
	})
	mux.HandleFunc("/stream", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(http.StatusOK)
		for i := 0; i < 3; i++ {
			_, _ = io.WriteString(w, "part"+strconv.Itoa(i)+" ")
		}
		_, _ = io.WriteString(w, "end\n")
	})
	mux.HandleFunc("/trailers", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Trailer", "X-Checksum")
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "body")
		w.Header().Set("X-Checksum", "abc123")
	})
	mux.HandleFunc("/sse", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream; charset=utf-8")
		w.Header().Set("Cache-Control", "no-cache")
		w.WriteHeader(http.StatusOK)
		for i := 1; i <= 3; i++ {
			_, _ = fmt.Fprintf(w, "event: tick\nid: %d\ndata: value-%d\n\n", i, i)
		}
	})
	mux.HandleFunc("/trailer-echo", func(w http.ResponseWriter, r *http.Request) {
		_ = readAll(r)
		value := r.Trailer.Get("X-Check")
		if value == "" {
			value = "missing"
		}
		writeText(w, http.StatusOK, value)
	})
	mux.HandleFunc("/expect-ok", func(w http.ResponseWriter, r *http.Request) {
		body := readAll(r)
		writeText(w, http.StatusOK, "got "+strconv.Itoa(len(body)))
	})
	mux.HandleFunc("/expect-reject", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusExpectationFailed, "no")
	})
	mux.HandleFunc("/who", whoHandler)
	mux.HandleFunc("/gzip", compressionHandler)
	mux.HandleFunc("/cors", corsHandler)
	mux.HandleFunc("/secure", secureHandler)
	mux.HandleFunc("/basic", basicAuthHandler)
	mux.HandleFunc("/request-id", func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-Id")
		if id == "" {
			id = randomToken(8)
		}
		w.Header().Set("X-Request-Id", id)
		writeText(w, http.StatusOK, id)
	})
	mux.HandleFunc("/rate", func(w http.ResponseWriter, r *http.Request) {
		state.mu.Lock()
		state.rateHits++
		hits := state.rateHits
		state.mu.Unlock()
		if hits > 1 {
			w.Header().Set("Retry-After", "1000")
			writeText(w, http.StatusTooManyRequests, "too many requests")
			return
		}
		writeText(w, http.StatusOK, "ok")
	})
	mux.HandleFunc("/session/set", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "POST") {
			return
		}
		id := loadSession(state, w, r)
		state.mu.Lock()
		if state.sessions[id] == nil {
			state.sessions[id] = map[string]string{}
		}
		state.sessions[id]["uid"] = "42"
		state.mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/session/get", func(w http.ResponseWriter, r *http.Request) {
		if !allow(w, r, "GET") {
			return
		}
		id := loadSession(state, w, r)
		state.mu.Lock()
		uid, ok := state.sessions[id]["uid"]
		state.mu.Unlock()
		if ok {
			writeText(w, http.StatusOK, uid)
		} else {
			writeText(w, http.StatusOK, "none")
		}
	})
	mux.HandleFunc("/csrf/token", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, csrfToken(w, r))
	})
	mux.HandleFunc("/csrf/submit", func(w http.ResponseWriter, r *http.Request) {
		token := csrfToken(w, r)
		supplied := r.Header.Get("X-Csrf-Token")
		if token == "" || subtle.ConstantTimeCompare([]byte(token), []byte(supplied)) != 1 {
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			w.WriteHeader(http.StatusForbidden)
			_, _ = io.WriteString(w, "CSRF token invalid")
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/csrf/exempt", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/jwt/me", jwtHandler)
	guarded := websocket.Upgrader{CheckOrigin: func(r *http.Request) bool {
		return r.Header.Get("Origin") == "https://app.example.com"
	}}
	mux.HandleFunc("/ws/guarded", func(w http.ResponseWriter, r *http.Request) {
		conn, err := guarded.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for {
			kind, data, err := conn.ReadMessage()
			if err != nil {
				return
			}
			if err := conn.WriteMessage(kind, data); err != nil {
				return
			}
		}
	})
	mux.HandleFunc("/ws/echo", func(w http.ResponseWriter, r *http.Request) {
		upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for {
			kind, data, err := conn.ReadMessage()
			if err != nil {
				return
			}
			if err := conn.WriteMessage(kind, data); err != nil {
				return
			}
		}
	})
	mux.Handle("/", staticHandler(staticRoot))
	limited := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.ContentLength > 1<<20 {
			writeText(w, http.StatusRequestEntityTooLarge, "request body too large")
			return
		}
		mux.ServeHTTP(w, r)
	})
	return limited
}

func main() {
	h1Addr := flag.String("h1", "127.0.0.1:7830", "h1 address")
	h2cAddr := flag.String("h2c", "127.0.0.1:7831", "h2c address")
	tlsAddr := flag.String("tls", "127.0.0.1:7832", "TLS address")
	h3Addr := flag.String("h3", "127.0.0.1:7833", "HTTP/3 address")
	mtlsAddr := flag.String("mtls", "127.0.0.1:7834", "mutual-TLS address")
	caFile := flag.String("ca", "./test/tls_certificates/ca_cert.pem", "CA bundle for client certificates")
	certFile := flag.String("cert", "./test/tls_certificates/server_cert.pem", "certificate")
	keyFile := flag.String("key", "./test/tls_certificates/server_key.pem", "private key")
	staticRoot := flag.String("static-root", "./test/diff/server/fixtures/static", "static root")
	enableH3 := flag.Bool("h3-enable", true, "start HTTP/3")
	flag.Parse()

	certificate, err := tls.LoadX509KeyPair(*certFile, *keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load certificate: %v\n", err)
		os.Exit(1)
	}

	handler := makeHandler(newState(), *staticRoot)

	h1Listener, err := net.Listen("tcp", *h1Addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "h1 listen: %v\n", err)
		os.Exit(1)
	}
	h1 := &http.Server{Handler: handler, MaxHeaderBytes: 64 << 10}
	go func() { _ = h1.Serve(h1Listener) }()

	h2cListener, err := net.Listen("tcp", *h2cAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "h2c listen: %v\n", err)
		os.Exit(1)
	}
	h2cServer := &http.Server{Handler: h2c.NewHandler(handler, &http2.Server{}), MaxHeaderBytes: 64 << 10}
	go func() { _ = h2cServer.Serve(h2cListener) }()

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{certificate},
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"h2", "http/1.1"},
	}
	tlsServer := &http.Server{Handler: handler, TLSConfig: tlsConfig, MaxHeaderBytes: 64 << 10}
	if err := http2.ConfigureServer(tlsServer, &http2.Server{}); err != nil {
		fmt.Fprintf(os.Stderr, "tls h2: %v\n", err)
		os.Exit(1)
	}
	tlsListener, err := net.Listen("tcp", *tlsAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "tls listen: %v\n", err)
		os.Exit(1)
	}
	go func() { _ = tlsServer.Serve(tls.NewListener(tlsListener, tlsConfig)) }()

	// Mutual TLS: RequireAndVerifyClientCert makes an absent or untrusted
	// client certificate a handshake failure. /client-cert exposes the CN.
	caPEM, err := os.ReadFile(*caFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load ca: %v\n", err)
		os.Exit(1)
	}
	clientCAs := x509.NewCertPool()
	if !clientCAs.AppendCertsFromPEM(caPEM) {
		fmt.Fprintln(os.Stderr, "ca bundle is empty")
		os.Exit(1)
	}
	mtlsConfig := &tls.Config{
		Certificates: []tls.Certificate{certificate},
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"http/1.1"},
		ClientAuth:   tls.RequireAndVerifyClientCert,
		ClientCAs:    clientCAs,
	}
	mtlsServer := &http.Server{Handler: handler, MaxHeaderBytes: 64 << 10}
	mtlsListener, err := net.Listen("tcp", *mtlsAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "mtls listen: %v\n", err)
		os.Exit(1)
	}
	go func() { _ = mtlsServer.Serve(tls.NewListener(mtlsListener, mtlsConfig)) }()

	var h3Server *http3.Server
	var h3Conn net.PacketConn
	if *enableH3 {
		address, err := net.ResolveUDPAddr("udp", *h3Addr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "h3 resolve: %v\n", err)
			os.Exit(1)
		}
		h3Conn, err = net.ListenUDP("udp", address)
		if err != nil {
			fmt.Fprintf(os.Stderr, "h3 listen: %v\n", err)
			os.Exit(1)
		}
		h3Server = &http3.Server{
			TLSConfig: &tls.Config{
				Certificates: []tls.Certificate{certificate},
				MinVersion:   tls.VersionTLS13,
				NextProtos:   []string{"h3"},
			},
			Handler: handler,
		}
		go func() { _ = h3Server.Serve(h3Conn) }()
	}

	fmt.Printf("READY diff-server-go h1=%s h2c=%s tls=%s h3=%s mtls=%s\n", *h1Addr, *h2cAddr, *tlsAddr, *h3Addr,
		*mtlsAddr)
	os.Stdout.Sync()

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGINT, syscall.SIGTERM)
	<-signals
	_ = h1.Close()
	_ = h2cServer.Close()
	_ = tlsServer.Close()
	_ = mtlsServer.Close()
	if h3Server != nil {
		_ = h3Server.Close()
	}
	if h3Conn != nil {
		_ = h3Conn.Close()
	}
}
