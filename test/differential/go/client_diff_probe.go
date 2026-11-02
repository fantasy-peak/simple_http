// Go client probe for the new client features. Run against
// test/differential/go/client_diff_server.go (baseline) and against the C++ framework server
// (the independent server leg). Its PASS/FAIL lines are diffed with the C++
// probe's output.

package main

import (
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"os"
	"strings"
	"time"
)

var failed int

func check(ok bool, name, detail string) {
	if ok {
		fmt.Println("PASS " + name)
		return
	}
	failed++
	if detail == "" {
		fmt.Println("FAIL " + name)
	} else {
		fmt.Println("FAIL " + name + " [" + detail + "]")
	}
}

func readBody(resp *http.Response) (string, error) {
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	return string(data), err
}

func main() {
	base := "http://127.0.0.1:7802"
	if len(os.Args) > 1 {
		base = os.Args[1]
	}
	client := &http.Client{Timeout: 10 * time.Second}

	// A temp file for the disk-file upload checks.
	file, err := os.CreateTemp("", "simple_http_client_diff_*.bin")
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	path := file.Name()
	payload := make([]byte, 200000)
	for i := range payload {
		payload[i] = byte('a' + i%26)
	}
	_, _ = file.Write(payload)
	_ = file.Close()
	defer os.Remove(path)

	// Streaming multipart: field + in-memory file part.
	{
		pr, pw := io.Pipe()
		writer := multipart.NewWriter(pw)
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
			_ = pw.Close()
		}()
		req, _ := http.NewRequest(http.MethodPost, base+"/mp", pr)
		req.Header.Set("Content-Type", writer.FormDataContentType())
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		want := "field:title=hello|file:upload:notes.txt:text/plain:9"
		check(err == nil && resp != nil && resp.StatusCode == 200 && body == want,
			"streaming multipart field+file", body)
	}

	// Streaming multipart: disk file.
	{
		pr, pw := io.Pipe()
		writer := multipart.NewWriter(pw)
		go func() {
			part, err := writer.CreateFormFile("up", "blob.bin")
			if err == nil {
				f, err := os.Open(path)
				if err == nil {
					_, _ = io.Copy(part, f)
					_ = f.Close()
				}
			}
			_ = writer.Close()
			_ = pw.Close()
		}()
		req, _ := http.NewRequest(http.MethodPost, base+"/mp", pr)
		req.Header.Set("Content-Type", writer.FormDataContentType())
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		want := "file:up:blob.bin:application/octet-stream:200000"
		check(err == nil && resp != nil && resp.StatusCode == 200 && body == want,
			"streaming multipart disk file", body)
	}

	// Raw file body upload.
	{
		f, _ := os.Open(path)
		req, _ := http.NewRequest(http.MethodPut, base+"/echo", f)
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		check(err == nil && resp != nil && resp.StatusCode == 200 && body == string(payload),
			"raw file body upload", "")
		_ = f.Close()
	}

	// Automatic Expect: 100-continue.
	{
		req, _ := http.NewRequest(http.MethodPost, base+"/expect-ok", strings.NewReader("hello!"))
		req.Header.Set("Expect", "100-continue")
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		check(err == nil && resp != nil && resp.StatusCode == 200 && body == "got 6",
			"auto 100-continue", body)
	}

	// A final response instead of a 100.
	{
		req, _ := http.NewRequest(http.MethodPost, base+"/expect-reject", strings.NewReader("body"))
		req.Header.Set("Expect", "100-continue")
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		check(err == nil && resp != nil && resp.StatusCode == 417 && body == "no",
			"expect rejected by final response", body)
	}

	// HTTP/1.1 request trailers. A known Content-Length would make Go send a
	// fixed-length body, which cannot carry trailers; -1 forces chunked.
	{
		req, _ := http.NewRequest(http.MethodPost, base+"/trailer-echo", strings.NewReader("body"))
		req.ContentLength = -1
		req.Trailer = http.Header{"X-Check": []string{"yes"}}
		resp, err := client.Do(req)
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		check(err == nil && resp != nil && resp.StatusCode == 200 && body == "yes",
			"request trailers", body)
	}

	// 429 with Retry-After: two rejects, then 200.
	{
		statuses := make([]int, 0, 3)
		body := ""
		for attempt := 0; attempt < 4; attempt++ {
			resp, err := client.Get(base + "/retry-status")
			if err != nil {
				break
			}
			body, _ = readBody(resp)
			statuses = append(statuses, resp.StatusCode)
			if resp.StatusCode != 429 {
				break
			}
			time.Sleep(0)
		}
		got := fmt.Sprint(statuses)
		check(body == "ok" && got == "[429 429 200]", "429 Retry-After retry", got+" "+body)
	}

	// JWKS fetch.
	{
		resp, err := client.Get(base + "/jwks")
		body := ""
		if err == nil {
			body, err = readBody(resp)
		}
		check(err == nil && resp != nil && resp.StatusCode == 200 && strings.Contains(body, `"keys"`),
			"JWKS fetch", body)
	}

	if failed != 0 {
		os.Exit(1)
	}
}
