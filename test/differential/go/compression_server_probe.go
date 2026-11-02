// Compare server-side request decompression between the Go reference server and
// the C++ fixture. Both expose /decode (opt-in decompression) and /raw (no
// decompression); this probe sends the same requests to both and checks that
// status, headers and body agree.
//
//   go run compression_server_probe.go http://127.0.0.1:7810 http://127.0.0.1:7788
package main

import (
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
)

var failed int

func check(name string, ok bool) {
	if ok {
		fmt.Printf("PASS  %s\n", name)
		return
	}
	failed++
	fmt.Printf("FAIL  %s\n", name)
}

func gzipBytes(body []byte) []byte {
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	_, _ = zw.Write(body)
	_ = zw.Close()
	return buf.Bytes()
}

func post(base, path string, body []byte, headers map[string]string) (*http.Response, []byte) {
	req, err := http.NewRequest("POST", base+path, bytes.NewReader(body))
	if err != nil {
		return nil, nil
	}
	for name, value := range headers {
		req.Header.Set(name, value)
	}
	resp, err := (&http.Client{}).Do(req)
	if err != nil {
		return nil, nil
	}
	defer resp.Body.Close()
	out, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, nil
	}
	return resp, out
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: compression_server_probe <go-base> <cpp-base>")
		os.Exit(2)
	}
	goBase := strings.TrimRight(os.Args[1], "/")
	cppBase := strings.TrimRight(os.Args[2], "/")
	decoded := []byte("the same body reaches both servers")

	// 1. gzip is decoded identically.
	{
		wire := gzipBytes(decoded)
		gr, gb := post(goBase, "/decode", wire, map[string]string{"Content-Encoding": "gzip"})
		cr, cb := post(cppBase, "/decode", wire, map[string]string{"Content-Encoding": "gzip"})
		check("gzip /decode: both decode to the same body",
			gr != nil && cr != nil && gr.StatusCode == 200 && cr.StatusCode == 200 && bytes.Equal(gb, cb) &&
				bytes.Contains(gb, decoded))
	}

	// 2. unsupported coding is 415 + the same Accept-Encoding advice.
	{
		gr, _ := post(goBase, "/decode", []byte("x"), map[string]string{"Content-Encoding": "deflate"})
		cr, _ := post(cppBase, "/decode", []byte("x"), map[string]string{"Content-Encoding": "deflate"})
		check("deflate /decode: both are 415 with Accept-Encoding",
			gr != nil && cr != nil && gr.StatusCode == 415 && cr.StatusCode == 415 &&
				strings.Contains(gr.Header.Get("Accept-Encoding"), "gzip") &&
				strings.Contains(cr.Header.Get("Accept-Encoding"), "gzip"))
	}

	// 3. malformed gzip is 400 on both.
	{
		gr, _ := post(goBase, "/decode", []byte("not gzip"), map[string]string{"Content-Encoding": "gzip"})
		cr, _ := post(cppBase, "/decode", []byte("not gzip"), map[string]string{"Content-Encoding": "gzip"})
		check("malformed gzip /decode: both are 400",
			gr != nil && cr != nil && gr.StatusCode == 400 && cr.StatusCode == 400)
	}

	// 4. decoded-size budget is 413 on both.
	{
		wire := gzipBytes(bytes.Repeat([]byte("z"), 2*1024*1024))
		gr, _ := post(goBase, "/decode", wire, map[string]string{"Content-Encoding": "gzip"})
		cr, _ := post(cppBase, "/decode", wire, map[string]string{"Content-Encoding": "gzip"})
		check("oversize /decode: both are 413",
			gr != nil && cr != nil && gr.StatusCode == 413 && cr.StatusCode == 413)
	}

	// 5. Without the middleware, both servers hand the raw compressed bytes to
	// the handler and leave Content-Encoding alone.
	{
		wire := gzipBytes(decoded)
		gr, gb := post(goBase, "/raw", wire, map[string]string{"Content-Encoding": "gzip"})
		cr, cb := post(cppBase, "/raw", wire, map[string]string{"Content-Encoding": "gzip"})
		check("/raw: both servers forward the compressed bytes untouched",
			gr != nil && cr != nil && gr.StatusCode == 200 && cr.StatusCode == 200 && bytes.Equal(gb, cb) &&
				bytes.HasSuffix(gb, wire) && bytes.Contains(gb, []byte("ce=gzip;")))
	}

	if failed != 0 {
		os.Exit(1)
	}
}
