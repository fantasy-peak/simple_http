// Go reference server for the client differential probe.
//
// It implements the server-side semantics the C++ and Go client probes exercise:
// streaming multipart parsing, Expect: 100-continue, request trailers and
// Retry-After statuses. The same server also runs as the baseline for the Go
// probe, so a diff between the two probe outputs is a client-behavior diff.
//
//	go run test/go/client_diff_server.go 7802

package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"sync/atomic"
)

func writeText(w http.ResponseWriter, status int, body string) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(status)
	_, _ = w.Write([]byte(body))
}

func main() {
	port := "7802"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	var retryHits atomic.Int32
	mux := http.NewServeMux()

	mux.HandleFunc("GET /world", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, "world")
	})
	mux.HandleFunc("POST /world", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, "world")
	})
	mux.HandleFunc("PUT /echo", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		writeText(w, http.StatusOK, string(body))
	})
	mux.HandleFunc("POST /echo", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		writeText(w, http.StatusOK, string(body))
	})

	mux.HandleFunc("POST /mp", func(w http.ResponseWriter, r *http.Request) {
		reader, err := r.MultipartReader()
		if err != nil {
			writeText(w, http.StatusBadRequest, "not multipart")
			return
		}
		out := ""
		for {
			part, err := reader.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				writeText(w, http.StatusBadRequest, "bad multipart")
				return
			}
			data, err := io.ReadAll(part)
			if err != nil {
				writeText(w, http.StatusBadRequest, "bad part")
				return
			}
			if out != "" {
				out += "|"
			}
			if part.FileName() == "" {
				out += "field:" + part.FormName() + "=" + string(data)
			} else {
				contentType := part.Header.Get("Content-Type")
				out += "file:" + part.FormName() + ":" + part.FileName() + ":" + contentType + ":" +
					strconv.Itoa(len(data))
			}
		}
		writeText(w, http.StatusOK, out)
	})

	// Go's server emits 100 Continue automatically on the first Body read.
	mux.HandleFunc("POST /expect-ok", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		writeText(w, http.StatusOK, "got "+strconv.Itoa(len(body)))
	})
	mux.HandleFunc("POST /expect-reject", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusExpectationFailed, "no")
	})

	mux.HandleFunc("POST /trailer-echo", func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.ReadAll(r.Body)
		value := r.Trailer.Get("x-check")
		if value == "" {
			value = "missing"
		}
		writeText(w, http.StatusOK, value)
	})

	mux.HandleFunc("GET /retry-status", func(w http.ResponseWriter, r *http.Request) {
		if retryHits.Add(1) <= 2 {
			w.Header().Set("Retry-After", "0")
			writeText(w, http.StatusTooManyRequests, "retry")
			return
		}
		writeText(w, http.StatusOK, "ok")
	})

	mux.HandleFunc("GET /jwks", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"keys":[]}`))
	})

	addr := "127.0.0.1:" + port
	fmt.Printf("go client reference server on %s\n", addr)
	if err := http.ListenAndServe(addr, mux); err != nil && err != http.ErrServerClosed {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
