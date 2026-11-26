// Runtime check: how does the reference Go HTTP/2 server treat an extended
// CONNECT whose :protocol the application does not implement?
//
// Run with GODEBUG=http2xconnect=1: x/net/http2 disables extended CONNECT by
// default (it does not advertise SETTINGS_ENABLE_CONNECT_PROTOCOL), and a
// :protocol sent in that state is a stream PROTOCOL_ERROR. With the setting
// enabled it dispatches the request to the application (method CONNECT,
// ":protocol" visible in the header); no 501 is generated at the framing layer.
//
// This is the reference for the C++ engine's behaviour: it dispatches a CONNECT
// as an ordinary request and lets the application decide.

package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"time"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/hpack"
)

func rawFrame(w io.Writer, typ http2.FrameType, flags http2.Flags, streamID uint32, payload []byte) error {
	return http2.NewFramer(w, nil).WriteRawFrame(typ, flags, streamID, payload)
}

func main() {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	defer ln.Close()

	var seenMethod, seenProtocol string
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seenMethod = r.Method
		seenProtocol = r.Header.Get(":protocol")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "ok")
	})
	base := &http.Server{Handler: handler}
	srv := &http2.Server{}
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		srv.ServeConn(c, &http2.ServeConnOpts{BaseConfig: base, Handler: handler})
	}()

	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))

	if _, err := io.WriteString(conn, http2.ClientPreface); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	_ = rawFrame(conn, http2.FrameSettings, 0, 0, nil)

	var block bytes.Buffer
	enc := hpack.NewEncoder(&block)
	for _, f := range []hpack.HeaderField{
		{Name: ":method", Value: "CONNECT"},
		{Name: ":protocol", Value: "foo"},
		{Name: ":scheme", Value: "https"},
		{Name: ":path", Value: "/x"},
		{Name: ":authority", Value: "example.com"},
	} {
		_ = enc.WriteField(f)
	}
	if err := rawFrame(conn, http2.FrameHeaders,
		http2.FlagHeadersEndHeaders|http2.FlagHeadersEndStream, 1, block.Bytes()); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	fr := http2.NewFramer(io.Discard, bufio.NewReader(conn))
	fr.MaxHeaderListSize = 1 << 20
	fr.ReadMetaHeaders = hpack.NewDecoder(4096, nil) // decode the response head
	status := ""
	for {
		f, err := fr.ReadFrame()
		if err != nil {
			break
		}
		switch f := f.(type) {
		case *http2.MetaHeadersFrame:
			for _, hf := range f.Fields {
				if hf.Name == ":status" {
					status = hf.Value
				}
			}
		case *http2.GoAwayFrame:
			fmt.Printf("[go] extended CONNECT :protocol=foo -> GOAWAY 0x%x (table=PROTOCOL_ERROR)\n", uint32(f.ErrCode))
			fmt.Println("[go] note: this is the default (extended CONNECT disabled) behaviour")
			os.Exit(1)
		case *http2.RSTStreamFrame:
			fmt.Printf("[go] extended CONNECT :protocol=foo -> RST_STREAM 0x%x (table=PROTOCOL_ERROR)\n",
				uint32(f.ErrCode))
			fmt.Println("[go] note: this is the default (extended CONNECT disabled) behaviour")
			os.Exit(1)
		}
		if status != "" {
			break
		}
	}

	fmt.Printf("[go] extended CONNECT :protocol=foo -> dispatched=%v method=%q protocol=%q status=%q\n",
		seenMethod != "", seenMethod, seenProtocol, status)
	if seenMethod != "CONNECT" || seenProtocol != "foo" || status != "200" {
		fmt.Println("[go] unexpected: the reference server did not dispatch the request")
		os.Exit(1)
	}
	fmt.Println("[go] OK: the application decides; no 501 at the framing layer")
}
