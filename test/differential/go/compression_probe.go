// Go net/http client probe for the compression differential.
//
// It prints the same PASS/FAIL lines as test/client/probes/compression_diff_probe.cpp
// so the two outputs can be diffed line by line. Both run against
// compression_server.go, which lets the probe observe whether the client
// injected Accept-Encoding and whether it decoded the body.
//
//   go run compression_probe.go http://127.0.0.1:7810
package main

import (
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

func request(client *http.Client, method, url string, headers map[string]string) (*http.Response, []byte) {
	req, err := http.NewRequest(method, url, nil)
	if err != nil {
		return nil, nil
	}
	for name, value := range headers {
		req.Header.Set(name, value)
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, nil
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, nil
	}
	return resp, body
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: compression_probe <base-url>")
		os.Exit(2)
	}
	base := strings.TrimRight(os.Args[1], "/")
	defaultClient := &http.Client{}
	noCompression := &http.Client{Transport: &http.Transport{DisableCompression: true}}

	resp, body := request(defaultClient, "GET", base+"/plain", nil)
	check("plain: identity passes through",
		resp != nil && resp.StatusCode == 200 && string(body) == "plain" && !resp.Uncompressed &&
			resp.Header.Get("Content-Encoding") == "")

	resp, body = request(defaultClient, "GET", base+"/gzip", nil)
	check("gzip: default client decodes and marks Uncompressed",
		resp != nil && resp.StatusCode == 200 && string(body) == "gzip-body" && resp.Uncompressed)
	check("gzip: decoded response drops Content-Encoding and Content-Length",
		resp != nil && resp.Header.Get("Content-Encoding") == "" && resp.Header.Get("Content-Length") == "")

	resp, body = request(defaultClient, "GET", base+"/gzip", map[string]string{"Accept-Encoding": "gzip"})
	check("gzip: explicit Accept-Encoding keeps the body encoded",
		resp != nil && !resp.Uncompressed && resp.Header.Get("Content-Encoding") == "gzip" &&
			string(body) != "gzip-body")

	resp, body = request(defaultClient, "GET", base+"/deflate", nil)
	check("deflate: not auto-decoded",
		resp != nil && !resp.Uncompressed && resp.Header.Get("Content-Encoding") == "deflate" &&
			string(body) != "deflate-body")

	resp, body = request(defaultClient, "GET", base+"/multi", nil)
	check("multi-coding: not auto-decoded",
		resp != nil && !resp.Uncompressed && resp.Header.Get("Content-Encoding") == "gzip, br" &&
			string(body) != "multi-body")

	resp, _ = request(defaultClient, "HEAD", base+"/gzip-head", nil)
	check("HEAD: no automatic Accept-Encoding",
		resp != nil && !resp.Uncompressed && resp.Header.Get("X-Saw-Accept-Encoding") == "no")

	resp, _ = request(defaultClient, "GET", base+"/range", map[string]string{"Range": "bytes=0-3"})
	check("Range: no automatic Accept-Encoding",
		resp != nil && resp.Header.Get("X-Saw-Accept-Encoding") == "no")

	resp, body = request(noCompression, "GET", base+"/gzip", nil)
	check("disabled compression: no automatic Accept-Encoding",
		resp != nil && !resp.Uncompressed && resp.Header.Get("X-Saw-Accept-Encoding") == "no" &&
			resp.Header.Get("Content-Encoding") == "gzip" && string(body) != "gzip-body")

	resp, body = request(defaultClient, "GET", base+"/headers", nil)
	check("default client advertises gzip",
		resp != nil && strings.Contains(string(body), "accept-encoding: gzip"))

	if failed != 0 {
		os.Exit(1)
	}
}
