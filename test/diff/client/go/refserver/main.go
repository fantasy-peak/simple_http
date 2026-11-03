// Go reference server for the client differential.
//
// Both the C++ client and the Go client send the same JSON cases to this one
// process. Every endpoint either answers deterministically or echoes what the
// client actually did (method, headers, body, trailers), so a difference in
// the two clients' observations can only come from the clients.
//
//	7810  plaintext HTTP/1.1 (+ WebSocket /ws)
//	7811  TLS, ALPN h2 + http/1.1
//	7812  h2c (prior knowledge and Upgrade)
//	7813  HTTP/3 over QUIC
package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/signal"
	"sort"
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

type echoResponse struct {
	Method   string              `json:"method"`
	Path     string              `json:"path"`
	RawQuery string              `json:"raw_query"`
	Proto    string              `json:"proto"`
	Host     string              `json:"host"`
	Headers  map[string][]string `json:"headers"`
	Trailers map[string][]string `json:"trailers,omitempty"`
	BodyB64  string              `json:"body_b64"`
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	encoder := json.NewEncoder(w)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(value)
}

func writeText(w http.ResponseWriter, status int, body string) {
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(status)
	_, _ = io.WriteString(w, body)
}

func readAll(r *http.Request) []byte {
	data, _ := io.ReadAll(r.Body)
	return data
}

func echo(w http.ResponseWriter, r *http.Request) {
	body := readAll(r)
	// Echo application-visible headers only. Hop-by-hop fields and the
	// redirect Referer are client transport policy, not request semantics; the
	// redirect cases compare method/body, and a dedicated case pins Referer.
	hidden := map[string]bool{
		"connection": true, "keep-alive": true, "proxy-connection": true,
		"transfer-encoding": true, "upgrade": true, "te": true, "trailer": true,
		"referer": true,
	}
	headers := make(map[string][]string, len(r.Header))
	for name, values := range r.Header {
		lower := strings.ToLower(name)
		if hidden[lower] {
			continue
		}
		headers[lower] = values
	}
	trailers := make(map[string][]string)
	for name, values := range r.Trailer {
		trailers[strings.ToLower(name)] = values
	}
	writeJSON(w, http.StatusOK, echoResponse{
		Method:   r.Method,
		Path:     r.URL.Path,
		RawQuery: r.URL.RawQuery,
		Proto:    r.Proto,
		Host:     r.Host,
		Headers:  headers,
		Trailers: trailers,
		BodyB64:  base64.StdEncoding.EncodeToString(body),
	})
}

func gzipBytes(data []byte) []byte {
	var buffer bytes.Buffer
	writer := gzip.NewWriter(&buffer)
	_, _ = writer.Write(data)
	_ = writer.Close()
	return buffer.Bytes()
}

func multipartEcho(w http.ResponseWriter, r *http.Request) {
	reader, err := r.MultipartReader()
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "not multipart: " + err.Error()})
		return
	}
	type part struct {
		Name        string `json:"name"`
		Filename    string `json:"filename"`
		ContentType string `json:"content_type"`
		Size        int    `json:"size"`
		BodyB64     string `json:"body_b64"`
	}
	parts := []part{}
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
		parts = append(parts, part{
			Name:        item.FormName(),
			Filename:    item.FileName(),
			ContentType: item.Header.Get("Content-Type"),
			Size:        len(data),
			BodyB64:     base64.StdEncoding.EncodeToString(data),
		})
		_ = item.Close()
	}
	writeJSON(w, http.StatusOK, parts)
}

func stream(w http.ResponseWriter, r *http.Request) {
	flusher, _ := w.(http.Flusher)
	w.Header().Set("Content-Type", "text/plain")
	w.WriteHeader(http.StatusOK)
	for i := 0; i < 3; i++ {
		_, _ = io.WriteString(w, fmt.Sprintf("part%d ", i))
		if flusher != nil {
			flusher.Flush()
		}
	}
	_, _ = io.WriteString(w, "end\n")
}

func sse(w http.ResponseWriter, r *http.Request) {
	flusher, _ := w.(http.Flusher)
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	for i := 1; i <= 3; i++ {
		_, _ = fmt.Fprintf(w, "id: %d\nevent: tick\ndata: value-%d\n\n", i, i)
		if flusher != nil {
			flusher.Flush()
		}
	}
}

// bufferedConn lets the HTTP CONNECT handler hand any bytes already buffered
// by bufio (the TLS ClientHello often rides in the same packet as the CONNECT
// head) to the spliced upstream.
type bufferedConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c *bufferedConn) Read(p []byte) (int, error) { return c.reader.Read(p) }

func splice(a, b net.Conn) {
	done := make(chan struct{}, 2)
	copyOne := func(dst, src net.Conn) {
		_, _ = io.Copy(dst, src)
		if tcp, ok := dst.(*net.TCPConn); ok {
			_ = tcp.CloseWrite()
		}
		done <- struct{}{}
	}
	go copyOne(a, b)
	go copyOne(b, a)
	<-done
	<-done
	_ = a.Close()
	_ = b.Close()
}

func serveHTTPConnect(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	request, err := http.ReadRequest(reader)
	if err != nil || request.Method != http.MethodConnect {
		_, _ = io.WriteString(conn, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
		return
	}
	upstream, err := net.DialTimeout("tcp", request.Host, 5*time.Second)
	if err != nil {
		_, _ = io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
		return
	}
	if _, err := io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
		_ = upstream.Close()
		return
	}
	splice(&bufferedConn{Conn: conn, reader: reader}, upstream)
}

func acceptLoop(listener net.Listener, handler func(net.Conn)) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		go handler(conn)
	}
}

func serveSOCKS5(conn net.Conn) {
	defer conn.Close()
	header := make([]byte, 2)
	if _, err := io.ReadFull(conn, header); err != nil || header[0] != 0x05 {
		return
	}
	methods := make([]byte, int(header[1]))
	if _, err := io.ReadFull(conn, methods); err != nil {
		return
	}
	if _, err := conn.Write([]byte{0x05, 0x00}); err != nil { // no authentication
		return
	}
	request := make([]byte, 4)
	if _, err := io.ReadFull(conn, request); err != nil || request[0] != 0x05 || request[1] != 0x01 {
		return
	}
	var host string
	switch request[3] {
	case 0x01: // IPv4
		address := make([]byte, 4)
		if _, err := io.ReadFull(conn, address); err != nil {
			return
		}
		host = net.IP(address).String()
	case 0x03: // domain
		length := make([]byte, 1)
		if _, err := io.ReadFull(conn, length); err != nil {
			return
		}
		name := make([]byte, int(length[0]))
		if _, err := io.ReadFull(conn, name); err != nil {
			return
		}
		host = string(name)
	case 0x04: // IPv6
		address := make([]byte, 16)
		if _, err := io.ReadFull(conn, address); err != nil {
			return
		}
		host = net.IP(address).String()
	default:
		return
	}
	portBytes := make([]byte, 2)
	if _, err := io.ReadFull(conn, portBytes); err != nil {
		return
	}
	target := net.JoinHostPort(host, strconv.Itoa(int(portBytes[0])<<8|int(portBytes[1])))
	upstream, err := net.DialTimeout("tcp", target, 5*time.Second)
	if err != nil {
		_, _ = conn.Write([]byte{0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	if _, err := conn.Write([]byte{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0}); err != nil {
		_ = upstream.Close()
		return
	}
	splice(conn, upstream)
}

func main() {
	h1Addr := flag.String("h1", "127.0.0.1:7810", "plaintext HTTP/1.1 address")
	tlsAddr := flag.String("tls", "127.0.0.1:7811", "TLS h2/h1 address")
	h2cAddr := flag.String("h2c", "127.0.0.1:7812", "h2c address")
	h3Addr := flag.String("h3", "127.0.0.1:7813", "HTTP/3 address")
	mtlsAddr := flag.String("mtls", "127.0.0.1:7814", "mutual-TLS HTTP/1.1 address")
	certFile := flag.String("cert", "./test/tls_certificates/server_cert.pem", "TLS certificate")
	keyFile := flag.String("key", "./test/tls_certificates/server_key.pem", "TLS private key")
	caFile := flag.String("ca", "./test/tls_certificates/ca_cert.pem", "CA used to verify client certificates")
	httpProxyAddr := flag.String("http-proxy", "127.0.0.1:7816", "HTTP CONNECT proxy address")
	socksProxyAddr := flag.String("socks-proxy", "127.0.0.1:7817", "SOCKS5 proxy address")
	altAddr := flag.String("alt", "127.0.0.1:7815", "second plaintext origin for cross-origin redirects")
	enableH3 := flag.Bool("h3-enable", true, "start the HTTP/3 listener")
	flag.Parse()

	certificate, err := tls.LoadX509KeyPair(*certFile, *keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load certificate: %v\n", err)
		os.Exit(1)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/echo", echo)
	mux.HandleFunc("/status/", func(w http.ResponseWriter, r *http.Request) {
		code, _ := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/status/"))
		if code == 0 {
			code = 200
		}
		w.WriteHeader(code)
		_, _ = io.WriteString(w, "status")
	})
	mux.HandleFunc("/redirect/", func(w http.ResponseWriter, r *http.Request) {
		code, _ := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/redirect/"))
		if code == 0 {
			code = 302
		}
		w.Header().Set("Location", "/echo")
		w.WriteHeader(code)
	})
	mux.HandleFunc("/redirect-cross", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Location", "http://127.0.0.1:7815/echo")
		w.WriteHeader(http.StatusFound)
	})
	mux.HandleFunc("/redirect-chain/", func(w http.ResponseWriter, r *http.Request) {
		left, _ := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/redirect-chain/"))
		if left <= 0 {
			_, _ = io.WriteString(w, "done")
			return
		}
		w.Header().Set("Location", fmt.Sprintf("/redirect-chain/%d", left-1))
		w.WriteHeader(http.StatusFound)
	})
	mux.HandleFunc("/referer", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Referer") != "" {
			writeText(w, http.StatusOK, "yes")
			return
		}
		writeText(w, http.StatusOK, "no")
	})
	mux.HandleFunc("/redirect-to-referer", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Location", "/referer")
		w.WriteHeader(http.StatusFound)
	})
	mux.HandleFunc("/cookies/set", func(w http.ResponseWriter, r *http.Request) {
		http.SetCookie(w, &http.Cookie{Name: "a", Value: "1", Path: "/"})
		http.SetCookie(w, &http.Cookie{Name: "b", Value: "2", Path: "/deep"})
		http.SetCookie(w, &http.Cookie{Name: "c", Value: "3", Path: "/", HttpOnly: true})
		http.SetCookie(w, &http.Cookie{Name: "d", Value: "4", Path: "/", MaxAge: 60})
		http.SetCookie(w, &http.Cookie{Name: "e", Value: "5", Path: "/", Secure: true})
		http.SetCookie(w, &http.Cookie{Name: "f", Value: "6", Path: "/", Expires: time.Unix(0, 0)})
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/cookies", func(w http.ResponseWriter, r *http.Request) {
		// Return the jar state as a sorted name=value list. Cookie order on the
		// wire is not semantic (RFC 6265 §5.4), and C++/Go jars order
		// differently; the set of cookies is what the differential compares.
		pairs := []string{}
		for _, cookie := range r.Cookies() {
			pairs = append(pairs, cookie.Name+"="+cookie.Value)
		}
		sort.Strings(pairs)
		writeJSON(w, http.StatusOK, map[string][]string{"cookie": pairs})
	})
	mux.HandleFunc("/compress/gzip", func(w http.ResponseWriter, r *http.Request) {
		body := []byte(r.URL.Query().Get("body"))
		if !strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
			w.Header().Set("Content-Type", "text/plain")
			_, _ = w.Write(body)
			return
		}
		compressed := gzipBytes(body)
		w.Header().Set("Content-Type", "text/plain")
		w.Header().Set("Content-Encoding", "gzip")
		w.Header().Set("Vary", "Accept-Encoding")
		w.Header().Set("Content-Length", strconv.Itoa(len(compressed)))
		_, _ = w.Write(compressed)
	})
	mux.HandleFunc("/compress/identity", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, r.URL.Query().Get("body"))
	})
	var retryMu sync.Mutex
	retryCounters := map[string]int{}
	mux.HandleFunc("/retry/reset", func(w http.ResponseWriter, r *http.Request) {
		retryMu.Lock()
		retryCounters = map[string]int{}
		retryMu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/retry/", func(w http.ResponseWriter, r *http.Request) {
		limit, _ := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/retry/"))
		retryMu.Lock()
		hits := retryCounters[r.URL.Path]
		retryCounters[r.URL.Path] = hits + 1
		retryMu.Unlock()
		if hits < limit {
			w.Header().Set("Retry-After", "0")
			w.Header().Set("Content-Type", "text/plain")
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = io.WriteString(w, "retry")
			return
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, "ok")
	})
	mux.HandleFunc("/trailers", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Trailer", "X-Checksum")
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "body")
		w.Header().Set("X-Checksum", "abc123")
	})
	mux.HandleFunc("/sse", sse)
	mux.HandleFunc("/sse-resume", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		start := 1
		if r.Header.Get("Last-Event-ID") == "2" {
			start = 3
		}
		for i := start; i <= 4; i++ {
			_, _ = fmt.Fprintf(w, "id: %d\ndata: value-%d\n\n", i, i)
		}
	})
	mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) {
		ms, _ := strconv.Atoi(r.URL.Query().Get("ms"))
		if ms > 0 {
			time.Sleep(time.Duration(ms) * time.Millisecond)
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, "slow")
	})
	mux.HandleFunc("/multipart", multipartEcho)
	mux.HandleFunc("/expect-ok", func(w http.ResponseWriter, r *http.Request) {
		body := readAll(r)
		w.Header().Set("Content-Type", "text/plain")
		_, _ = fmt.Fprintf(w, "got %d", len(body))
	})
	mux.HandleFunc("/expect-reject", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		w.WriteHeader(http.StatusExpectationFailed)
		_, _ = io.WriteString(w, "no")
	})
	mux.HandleFunc("/request-trailer", func(w http.ResponseWriter, r *http.Request) {
		_ = readAll(r)
		value := r.Trailer.Get("X-Check")
		if value == "" {
			value = "missing"
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, value)
	})
	mux.HandleFunc("/jwks", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"keys": []any{}})
	})
	mux.HandleFunc("/stream", stream)
	mux.HandleFunc("/large", func(w http.ResponseWriter, r *http.Request) {
		n, _ := strconv.Atoi(r.URL.Query().Get("n"))
		if n <= 0 {
			n = 1024
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, strings.Repeat("a", n))
	})
	mux.HandleFunc("/version", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, r.Proto)
	})
	var upgrader = websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
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
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		_, _ = io.WriteString(w, "root")
	})

	handler := mux

	// Bind every listener synchronously, then print READY. The runner waits for
	// this line instead of guessing from TCP probes (which cannot see QUIC).
	h1Listener, err := net.Listen("tcp", *h1Addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "h1 listen: %v\n", err)
		os.Exit(1)
	}
	h1 := &http.Server{Handler: handler}
	go func() {
		if err := h1.Serve(h1Listener); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "h1: %v\n", err)
		}
	}()

	altListener, err := net.Listen("tcp", *altAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "alt listen: %v\n", err)
		os.Exit(1)
	}
	altServer := &http.Server{Handler: handler}
	go func() {
		if err := altServer.Serve(altListener); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "alt: %v\n", err)
		}
	}()

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{certificate},
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"h2", "http/1.1"},
	}
	tlsServer := &http.Server{Handler: handler, TLSConfig: tlsConfig}
	if err := http2.ConfigureServer(tlsServer, &http2.Server{}); err != nil {
		fmt.Fprintf(os.Stderr, "h2: %v\n", err)
		os.Exit(1)
	}
	tlsListener, err := net.Listen("tcp", *tlsAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "tls listen: %v\n", err)
		os.Exit(1)
	}
	go func() {
		if err := tlsServer.Serve(tls.NewListener(tlsListener, tlsConfig)); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "tls: %v\n", err)
		}
	}()

	h2cListener, err := net.Listen("tcp", *h2cAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "h2c listen: %v\n", err)
		os.Exit(1)
	}
	h2cServer := &http.Server{Handler: h2c.NewHandler(handler, &http2.Server{})}
	go func() {
		if err := h2cServer.Serve(h2cListener); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "h2c: %v\n", err)
		}
	}()

	// Mutual TLS: the client must present a certificate signed by the test CA.
	caPEM, err := os.ReadFile(*caFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load ca: %v\n", err)
		os.Exit(1)
	}
	clientCAs := x509.NewCertPool()
	if !clientCAs.AppendCertsFromPEM(caPEM) {
		fmt.Fprintf(os.Stderr, "parse ca: %v\n", *caFile)
		os.Exit(1)
	}
	mtlsConfig := &tls.Config{
		Certificates: []tls.Certificate{certificate},
		ClientAuth:   tls.RequireAndVerifyClientCert,
		ClientCAs:    clientCAs,
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"http/1.1"},
	}
	mtlsListener, err := net.Listen("tcp", *mtlsAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "mtls listen: %v\n", err)
		os.Exit(1)
	}
	mtlsServer := &http.Server{Handler: handler}
	go func() {
		if err := mtlsServer.Serve(tls.NewListener(mtlsListener, mtlsConfig)); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "mtls: %v\n", err)
		}
	}()

	httpProxyListener, err := net.Listen("tcp", *httpProxyAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "http proxy listen: %v\n", err)
		os.Exit(1)
	}
	go acceptLoop(httpProxyListener, serveHTTPConnect)
	socksProxyListener, err := net.Listen("tcp", *socksProxyAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "socks proxy listen: %v\n", err)
		os.Exit(1)
	}
	go acceptLoop(socksProxyListener, serveSOCKS5)

	var h3Server *http3.Server
	var h3Conn net.PacketConn
	if *enableH3 {
		h3Config := &tls.Config{
			Certificates: []tls.Certificate{certificate},
			MinVersion:   tls.VersionTLS13,
			NextProtos:   []string{"h3"},
		}
		h3AddrResolved, err := net.ResolveUDPAddr("udp", *h3Addr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "h3 resolve: %v\n", err)
			os.Exit(1)
		}
		h3Conn, err = net.ListenUDP("udp", h3AddrResolved)
		if err != nil {
			fmt.Fprintf(os.Stderr, "h3 listen: %v\n", err)
			os.Exit(1)
		}
		h3Server = &http3.Server{TLSConfig: h3Config, Handler: handler}
		go func() {
			if err := h3Server.Serve(h3Conn); err != nil && err != http.ErrServerClosed {
				fmt.Fprintf(os.Stderr, "h3: %v\n", err)
			}
		}()
	}

	fmt.Printf("READY client-reference h1=%s tls=%s h2c=%s h3=%s mtls=%s alt=%s http-proxy=%s socks-proxy=%s\n",
		*h1Addr, *tlsAddr, *h2cAddr, *h3Addr, *mtlsAddr, *altAddr, *httpProxyAddr, *socksProxyAddr)
	os.Stdout.Sync()

	signalChannel := make(chan os.Signal, 1)
	signal.Notify(signalChannel, syscall.SIGINT, syscall.SIGTERM)
	<-signalChannel
	_ = h1.Close()
	_ = tlsServer.Close()
	_ = h2cServer.Close()
	_ = mtlsServer.Close()
	_ = altServer.Close()
	_ = httpProxyListener.Close()
	_ = socksProxyListener.Close()
	if h3Server != nil {
		_ = h3Server.Close()
	}
	if h3Conn != nil {
		_ = h3Conn.Close()
	}
}
