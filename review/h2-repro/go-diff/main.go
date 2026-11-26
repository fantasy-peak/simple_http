// Runtime differential for P1 against the reference Go HTTP/2 server
// (golang.org/x/net/http2).
//
// Two HPACK blocks are fed to a real Go server over a real TCP connection, each
// on stream 1 of a fresh connection:
//
//   * malformed: a decoded field list larger than MAX_HEADER_LIST_SIZE,
//     followed by a malformed representation (indexed field, index 0).
//   * wellformed: the same list, without the malformed tail.
//
// The question is the RFC 9113 classification: is crossing the decoded list cap
// a stream-level condition, while a malformed HPACK block is a connection-level
// COMPRESSION_ERROR -- even when both happen in the same block? The Go server
// answers that with its actual behavior:
//
//   malformed   -> GOAWAY(COMPRESSION_ERROR = 0x9)
//   wellformed  -> no GOAWAY(0x9); the request is refused at stream level
//                  (431/400/RST_STREAM, depending on where it fails first)
//
// Build/run (GOPATH mode; the module cache is used read-only):
//   review/h2-repro/run-go-diff.sh

package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"time"

	"golang.org/x/net/http2"
)

const errCodeCompression = 0x9

type result struct {
	goaway     bool
	goawayCode uint32
	status     string
	streamErr  string
	notes      []string
}

func rawFrame(w io.Writer, typ http2.FrameType, flags http2.Flags, streamID uint32, payload []byte) error {
	return http2.NewFramer(w, nil).WriteRawFrame(typ, flags, streamID, payload)
}

func runCase(block []byte, timeout time.Duration) (result, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return result{}, err
	}
	defer ln.Close()

	// Same cap the C++ engine advertises by default: 64 KiB. Go converts the
	// net/http value by adding 32 bytes for each of ~10 typical headers, so the
	// h2 MAX_HEADER_LIST_SIZE ends up at 65856.
	base := &http.Server{
		MaxHeaderBytes: 64 * 1024,
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("x-handler", "ran")
			w.WriteHeader(http.StatusOK)
		}),
	}
	srv := &http2.Server{}
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		srv.ServeConn(c, &http2.ServeConnOpts{BaseConfig: base, Handler: base.Handler})
	}()

	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		return result{}, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(timeout))

	if _, err := io.WriteString(conn, http2.ClientPreface); err != nil {
		return result{}, err
	}
	if err := rawFrame(conn, http2.FrameSettings, 0, 0, nil); err != nil {
		return result{}, err
	}
	if err := rawFrame(conn, http2.FrameHeaders,
		http2.FlagHeadersEndHeaders|http2.FlagHeadersEndStream, 1, block); err != nil {
		return result{}, err
	}

	fr := http2.NewFramer(io.Discard, bufio.NewReader(conn))
	fr.MaxHeaderListSize = 1 << 20 // decode the server's response heads
	var out result
	for {
		f, err := fr.ReadFrame()
		if err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				break
			}
			out.notes = append(out.notes, "read_error: "+err.Error())
			break
		}
		switch f := f.(type) {
		case *http2.GoAwayFrame:
			out.goaway = true
			out.goawayCode = uint32(f.ErrCode)
			return out, nil
		case *http2.MetaHeadersFrame:
			for _, hf := range f.Fields {
				if hf.Name == ":status" {
					out.status = hf.Value
				}
			}
			out.notes = append(out.notes, "HEADERS")
		case *http2.RSTStreamFrame:
			out.streamErr = fmt.Sprintf("RST_STREAM(stream=%d, code=0x%x)", f.StreamID, uint32(f.ErrCode))
		case *http2.SettingsFrame:
			out.notes = append(out.notes, "SETTINGS")
		case *http2.WindowUpdateFrame:
			// ignored
		default:
			out.notes = append(out.notes, f.Header().Type.String())
		}
	}
	return out, nil
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <malformed-block> <wellformed-block>\n", os.Args[0])
		os.Exit(2)
	}
	timeout := 3 * time.Second

	malformed, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "read %s: %v\n", os.Args[1], err)
		os.Exit(2)
	}
	wellformed, err := os.ReadFile(os.Args[2])
	if err != nil {
		fmt.Fprintf(os.Stderr, "read %s: %v\n", os.Args[2], err)
		os.Exit(2)
	}

	mr, err := runCase(malformed, timeout)
	if err != nil {
		fmt.Fprintf(os.Stderr, "malformed case: %v\n", err)
		os.Exit(2)
	}
	wr, err := runCase(wellformed, timeout)
	if err != nil {
		fmt.Fprintf(os.Stderr, "wellformed case: %v\n", err)
		os.Exit(2)
	}

	fmt.Printf("[go] malformed : goaway=%v code=0x%x status=%q streamErr=%q notes=%v\n",
		mr.goaway, mr.goawayCode, mr.status, mr.streamErr, mr.notes)
	fmt.Printf("[go] wellformed: goaway=%v code=0x%x status=%q streamErr=%q notes=%v\n",
		wr.goaway, wr.goawayCode, wr.status, wr.streamErr, wr.notes)

	ok := mr.goaway && mr.goawayCode == errCodeCompression &&
		!(wr.goaway && wr.goawayCode == errCodeCompression)
	if !ok {
		fmt.Println("[go] FAIL: behavior does not match the RFC classification")
		os.Exit(1)
	}
	fmt.Println("[go] OK: malformed-after-cap is a connection-level COMPRESSION_ERROR;")
	fmt.Println("[go]     crossing the list cap on a well-formed block is not")
	os.Exit(0)
}
