// quic-go HTTP/3 0-RTT client for the differential. One normal GET warms the
// TLS session cache, then MethodGet0RTT is sent on a fresh connection.
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	if len(os.Args) < 2 {
		log.Fatal("usage: client <url>")
	}
	url := os.Args[1]
	expectEarly := true
	if len(os.Args) > 2 {
		expectEarly = os.Args[2] != "0"
	}
	client0RTT := true
	if len(os.Args) > 3 {
		client0RTT = os.Args[3] != "0"
	}
	tlsConf := &tls.Config{InsecureSkipVerify: true}
	if client0RTT {
		tlsConf.ClientSessionCache = tls.NewLRUClientSessionCache(4)
	}

	tr1 := &http3.Transport{TLSClientConfig: tlsConf}
	resp, err := (&http.Client{Transport: tr1}).Get(url)
	if err != nil {
		fmt.Printf("first_error=%v\n", err)
		os.Exit(1)
	}
	first, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	time.Sleep(800 * time.Millisecond)
	tr1.Close()

	tr2 := &http3.Transport{TLSClientConfig: tlsConf}
	req, _ := http.NewRequest(http3.MethodGet0RTT, url, nil)
	resp2, err := (&http.Client{Transport: tr2}).Do(req)
	if err != nil {
		fmt.Printf("second_error=%v\n", err)
		tr2.Close()
		os.Exit(1)
	}
	second, _ := io.ReadAll(resp2.Body)
	resp2.Body.Close()
	tr2.Close()

	fmt.Printf("first=%s\nsecond=%s\n", first, second)
	want := "normal"
	if expectEarly {
		want = "early"
	}
	if string(second) != want {
		os.Exit(1)
	}
}
