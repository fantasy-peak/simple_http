// Go net/http reference server for the compression differential.
//
// It exposes exactly the response shapes the probes need to distinguish:
// identity, gzip (auto-decodable by Go's Transport), deflate and a multi-coding
// list (not auto-decoded), HEAD, Range, and a header echo for checking whether
// the client injected Accept-Encoding.
//
//   go run compression_server.go 7810
package main

import (
	"compress/flate"
	"compress/gzip"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
)

func sawAcceptEncoding(r *http.Request) string {
	if r.Header.Get("Accept-Encoding") == "" {
		return "no"
	}
	return "yes"
}

func gzipBytes(body string) []byte {
	var buf strings.Builder
	zw := gzip.NewWriter(&buf)
	_, _ = zw.Write([]byte(body))
	_ = zw.Close()
	return []byte(buf.String())
}

func deflateBytes(body string) []byte {
	var buf strings.Builder
	zw, _ := flate.NewWriter(&buf, flate.DefaultCompression)
	_, _ = zw.Write([]byte(body))
	_ = zw.Close()
	return []byte(buf.String())
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: compression_server <port>")
		os.Exit(2)
	}
	mux := http.NewServeMux()

	mux.HandleFunc("/plain", func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte("plain"))
	})

	mux.HandleFunc("/gzip", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		w.Header().Set("Content-Encoding", "gzip")
		_, _ = w.Write(gzipBytes("gzip-body"))
	})

	mux.HandleFunc("/gzip-head", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		w.Header().Set("Content-Encoding", "gzip")
		if r.Method != http.MethodHead {
			_, _ = w.Write(gzipBytes("head-body"))
		}
	})

	mux.HandleFunc("/deflate", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		w.Header().Set("Content-Encoding", "deflate")
		_, _ = w.Write(deflateBytes("deflate-body"))
	})

	mux.HandleFunc("/multi", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		w.Header().Set("Content-Encoding", "gzip, br")
		_, _ = w.Write(gzipBytes("multi-body"))
	})

	mux.HandleFunc("/range", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		_, _ = w.Write([]byte("range-body"))
	})

	mux.HandleFunc("/headers", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Saw-Accept-Encoding", sawAcceptEncoding(r))
		w.Header().Set("Content-Type", "text/plain")
		for name, values := range r.Header {
			for _, value := range values {
				_, _ = fmt.Fprintf(w, "%s: %s\n", strings.ToLower(name), value)
			}
		}
	})

	// Server-side request decompression reference. The C++ fixture has the
	// same /decode and /raw routes; the server probe compares the two.
	mux.HandleFunc("/raw", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		ce := strings.ToLower(strings.TrimSpace(r.Header.Get("Content-Encoding")))
		if ce == "" {
			ce = "none"
		}
		_, _ = fmt.Fprintf(w, "ce=%s;", ce)
		_, _ = w.Write(body)
	})

	mux.HandleFunc("/decode", func(w http.ResponseWriter, r *http.Request) {
		const limit = 1024 * 1024
		ce := strings.ToLower(strings.TrimSpace(r.Header.Get("Content-Encoding")))
		if ce == "" || ce == "identity" {
			body, _ := io.ReadAll(r.Body)
			_, _ = fmt.Fprintf(w, "ce=%s;cl=none;", ce)
			_, _ = w.Write(body)
			return
		}
		if ce != "gzip" {
			w.Header().Set("Accept-Encoding", "gzip, br")
			http.Error(w, "unsupported content-encoding", http.StatusUnsupportedMediaType)
			return
		}
		zr, err := gzip.NewReader(r.Body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		defer zr.Close()
		body, err := io.ReadAll(io.LimitReader(zr, limit+1))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if len(body) > limit {
			http.Error(w, "decoded body too large", http.StatusRequestEntityTooLarge)
			return
		}
		_, _ = fmt.Fprintf(w, "ce=none;cl=none;")
		_, _ = w.Write(body)
	})

	addr := "127.0.0.1:" + os.Args[1]
	log.Fatal(http.ListenAndServe(addr, mux))
}
