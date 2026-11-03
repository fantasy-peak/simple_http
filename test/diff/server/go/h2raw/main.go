// Raw HTTP/2 assertions that Go's net/http cannot express.
//
// These are C++-only checks against the plaintext h2c listener (:7821):
//
//	h2.websocket_extended_connect
//	    RFC 8441 CONNECT + :protocol: websocket, then a masked WebSocket text
//	    frame over DATA and the echoed frame back.
//
//	h2.rapid_reset
//	    Open as many slow-handler streams as the server's advertised
//	    SETTINGS_MAX_CONCURRENT_STREAMS, RST_STREAM all of them (the reset frees
//	    each stream-table slot while the started handler keeps running), then
//	    open more into the gap. The excess must queue and, once the queue bound
//	    is exceeded, be answered with GOAWAY(ENHANCE_YOUR_CALM) instead of
//	    spawning handlers without bound.
//
// Go's net/http server has no extended-CONNECT WebSocket API and its Rapid
// Reset policy is different, so these are single-side assertions rather than
// differential cases. The runner reports them as server-h2raw checks.
package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"

	"golang.org/x/net/http2/hpack"
)

const (
	frameData         = 0x0
	frameHeaders      = 0x1
	frameRSTStream    = 0x3
	frameSettings     = 0x4
	framePing         = 0x6
	frameGoAway       = 0x7
	frameWindowUpdate = 0x8
	frameContinuation = 0x9

	flagEndStream  = 0x1
	flagAck        = 0x1
	flagEndHeaders = 0x4

	settingMaxConcurrentStreams  = 0x3
	settingEnableConnectProtocol = 0x8
	errorEnhanceYourCalm         = 0x0b
	errorCancel                  = 0x8
)

const clientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

type h2Conn struct {
	conn           net.Conn
	r              *bufio.Reader
	peerMaxStreams uint32
}

// maxConcurrentFromSettings reads SETTINGS_MAX_CONCURRENT_STREAMS out of a
// SETTINGS payload, falling back to the library default when the peer omitted
// it (RFC 9113: an absent setting means "no limit", but both reference servers
// always send one).
func maxConcurrentFromSettings(payload []byte) uint32 {
	limit := uint32(0)
	for i := 0; i+6 <= len(payload); i += 6 {
		id := binary.BigEndian.Uint16(payload[i : i+2])
		value := binary.BigEndian.Uint32(payload[i+2 : i+6])
		if id == settingMaxConcurrentStreams {
			limit = value
		}
	}
	if limit == 0 {
		limit = 200
	}
	return limit
}

func dial(addr string, timeout time.Duration) (*h2Conn, error) {
	conn, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, err
	}
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	c := &h2Conn{conn: conn, r: bufio.NewReader(conn)}
	if _, err := conn.Write([]byte(clientPreface)); err != nil {
		_ = conn.Close()
		return nil, err
	}
	// Advertise SETTINGS_ENABLE_CONNECT_PROTOCOL so extended CONNECT is legal.
	settings := []byte{0, byte(settingEnableConnectProtocol), 0, 0, 0, 1}
	if err := c.writeFrame(frameSettings, 0, 0, settings); err != nil {
		_ = conn.Close()
		return nil, err
	}
	// Wait for the peer's SETTINGS, then acknowledge it.
	for {
		typ, flags, _, payload, err := c.readFrame()
		if err != nil {
			_ = conn.Close()
			return nil, err
		}
		if typ == frameSettings && flags&flagAck == 0 {
			_ = c.writeFrame(frameSettings, flagAck, 0, nil)
			c.peerMaxStreams = maxConcurrentFromSettings(payload)
			return c, nil
		}
		if typ == frameGoAway {
			_ = conn.Close()
			return nil, fmt.Errorf("peer sent GOAWAY during settings")
		}
	}
}

func (c *h2Conn) writeFrame(typ, flags byte, stream uint32, payload []byte) error {
	header := make([]byte, 9)
	header[0] = byte(len(payload) >> 16)
	header[1] = byte(len(payload) >> 8)
	header[2] = byte(len(payload))
	header[3] = typ
	header[4] = flags
	binary.BigEndian.PutUint32(header[5:], stream&0x7fffffff)
	_, err := c.conn.Write(append(header, payload...))
	return err
}

func (c *h2Conn) readFrame() (byte, byte, uint32, []byte, error) {
	header := make([]byte, 9)
	if _, err := io.ReadFull(c.r, header); err != nil {
		return 0, 0, 0, nil, err
	}
	length := int(header[0])<<16 | int(header[1])<<8 | int(header[2])
	typ := header[3]
	flags := header[4]
	stream := binary.BigEndian.Uint32(header[5:]) & 0x7fffffff
	payload := make([]byte, length)
	if _, err := io.ReadFull(c.r, payload); err != nil {
		return 0, 0, 0, nil, err
	}
	return typ, flags, stream, payload, nil
}

func (c *h2Conn) ackPing(payload []byte) {
	_ = c.writeFrame(framePing, flagAck, 0, payload)
}

func encodeFields(fields []hpack.HeaderField) []byte {
	var buffer bytes.Buffer
	encoder := hpack.NewEncoder(&buffer)
	for _, field := range fields {
		_ = encoder.WriteField(field)
	}
	return buffer.Bytes()
}

func decodeFields(block []byte) ([]hpack.HeaderField, error) {
	var fields []hpack.HeaderField
	decoder := hpack.NewDecoder(4096, func(field hpack.HeaderField) {
		fields = append(fields, field)
	})
	if _, err := decoder.Write(block); err != nil {
		return nil, err
	}
	if err := decoder.Close(); err != nil {
		return nil, err
	}
	return fields, nil
}

func (c *h2Conn) readHeaders(stream uint32) ([]hpack.HeaderField, error) {
	var block []byte
	for {
		typ, flags, id, payload, err := c.readFrame()
		if err != nil {
			return nil, err
		}
		switch typ {
		case frameSettings:
			if flags&flagAck == 0 {
				_ = c.writeFrame(frameSettings, flagAck, 0, nil)
			}
		case framePing:
			c.ackPing(payload)
		case frameHeaders, frameContinuation:
			if id == stream {
				block = append(block, payload...)
				if flags&flagEndHeaders != 0 {
					return decodeFields(block)
				}
			}
		case frameRSTStream:
			if id == stream {
				return nil, fmt.Errorf("stream reset")
			}
		case frameGoAway:
			return nil, fmt.Errorf("GOAWAY before response")
		}
	}
}

func statusOf(fields []hpack.HeaderField) string {
	for _, field := range fields {
		if field.Name == ":status" {
			return field.Value
		}
	}
	return ""
}

func maskedTextFrame(text []byte) []byte {
	mask := []byte{0x11, 0x22, 0x33, 0x44}
	frame := []byte{0x81, 0x80 | byte(len(text))}
	frame = append(frame, mask...)
	for i, b := range text {
		frame = append(frame, b^mask[i%4])
	}
	return frame
}

func parseServerTextFrame(payload []byte) (string, bool) {
	if len(payload) < 2 || payload[0]&0x0f != 0x1 {
		return "", false
	}
	length := int(payload[1] & 0x7f)
	offset := 2
	if length == 126 {
		if len(payload) < 4 {
			return "", false
		}
		length = int(payload[2])<<8 | int(payload[3])
		offset = 4
	} else if length == 127 {
		if len(payload) < 10 {
			return "", false
		}
		length = int(binary.BigEndian.Uint64(payload[2:10]))
		offset = 10
	}
	if len(payload) < offset+length {
		return "", false
	}
	return string(payload[offset : offset+length]), true
}

func checkExtendedConnect(addr string) error {
	c, err := dial(addr, 3*time.Second)
	if err != nil {
		return err
	}
	defer c.conn.Close()

	key := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef"))
	fields := []hpack.HeaderField{
		{Name: ":method", Value: "CONNECT"},
		{Name: ":protocol", Value: "websocket"},
		{Name: ":scheme", Value: "http"},
		{Name: ":path", Value: "/ws/echo"},
		{Name: ":authority", Value: addr},
		{Name: "sec-websocket-version", Value: "13"},
		{Name: "sec-websocket-key", Value: key},
	}
	if err := c.writeFrame(frameHeaders, flagEndHeaders, 1, encodeFields(fields)); err != nil {
		return err
	}
	response, err := c.readHeaders(1)
	if err != nil {
		return err
	}
	if status := statusOf(response); status != "200" {
		return fmt.Errorf("extended CONNECT status %q", status)
	}
	if err := c.writeFrame(frameData, 0, 1, maskedTextFrame([]byte("hello"))); err != nil {
		return err
	}

	var data []byte
	for len(data) < 32 {
		typ, flags, id, payload, err := c.readFrame()
		if err != nil {
			return err
		}
		switch typ {
		case frameSettings:
			if flags&flagAck == 0 {
				_ = c.writeFrame(frameSettings, flagAck, 0, nil)
			}
		case framePing:
			c.ackPing(payload)
		case frameData:
			if id == 1 {
				data = append(data, payload...)
				if text, ok := parseServerTextFrame(data); ok {
					if text != "hello" {
						return fmt.Errorf("echo %q", text)
					}
					return nil
				}
			}
		case frameRSTStream:
			if id == 1 {
				return fmt.Errorf("stream reset")
			}
		case frameGoAway:
			return fmt.Errorf("GOAWAY before echo")
		}
	}
	return fmt.Errorf("no complete echo frame")
}

func checkRapidReset(addr string) error {
	c, err := dial(addr, 3*time.Second)
	if err != nil {
		return err
	}
	defer c.conn.Close()
	_ = c.conn.SetDeadline(time.Now().Add(10 * time.Second))

	var buffer bytes.Buffer
	encoder := hpack.NewEncoder(&buffer)
	writeHeaders := func(id uint32) error {
		buffer.Reset()
		for _, field := range []hpack.HeaderField{
			{Name: ":method", Value: "GET"},
			{Name: ":scheme", Value: "http"},
			{Name: ":authority", Value: addr},
			{Name: ":path", Value: "/delay?ms=2000"},
		} {
			if err := encoder.WriteField(field); err != nil {
				return err
			}
		}
		// HEADERS with END_STREAM starts the handler right away.
		return c.writeFrame(frameHeaders, flagEndHeaders|flagEndStream, id, buffer.Bytes())
	}

	// Fill the executing-handler cap with slow streams...
	open := c.peerMaxStreams
	next := uint32(1)
	for i := uint32(0); i < open; i, next = i+1, next+2 {
		if err := writeHeaders(next); err != nil {
			return err
		}
	}
	// ...then reset them all. The handlers keep running (the reset only frees
	// the stream-table slot), so the wire concurrency limit no longer blocks
	// fresh ids while the handler cap stays saturated.
	for id := uint32(1); id < next; id += 2 {
		if err := c.writeFrame(frameRSTStream, 0, id, []byte{0, 0, 0, errorCancel}); err != nil {
			return err
		}
	}
	// New streams must queue; with the fixture's small queue bound one of these
	// overflows it and the server must GOAWAY. Four is more than enough.
	for i := 0; i < 4; i, next = i+1, next+2 {
		if err := writeHeaders(next); err != nil {
			return err
		}
	}

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		typ, flags, _, payload, err := c.readFrame()
		if err != nil {
			return fmt.Errorf("connection ended without GOAWAY: %v", err)
		}
		switch typ {
		case frameSettings:
			if flags&flagAck == 0 {
				_ = c.writeFrame(frameSettings, flagAck, 0, nil)
			}
		case framePing:
			c.ackPing(payload)
		case frameGoAway:
			if len(payload) < 8 {
				return fmt.Errorf("short GOAWAY")
			}
			code := binary.BigEndian.Uint32(payload[4:8])
			if code != errorEnhanceYourCalm {
				return fmt.Errorf("GOAWAY code 0x%x, want 0x%x", code, errorEnhanceYourCalm)
			}
			return nil
		}
	}
	return fmt.Errorf("no GOAWAY(ENHANCE_YOUR_CALM)")
}

func main() {
	h2c := flag.String("h2c", "http://127.0.0.1:7821", "plaintext h2c base")
	flag.Parse()
	addr := strings.TrimPrefix(strings.TrimPrefix(*h2c, "http://"), "https://")

	failed := 0
	check := func(name string, err error) {
		if err == nil {
			fmt.Println("PASS " + name)
			return
		}
		failed++
		fmt.Printf("FAIL %s [%s]\n", name, err)
	}
	check("h2.websocket_extended_connect", checkExtendedConnect(addr))
	check("h2.rapid_reset", checkRapidReset(addr))
	if failed != 0 {
		os.Exit(1)
	}
}
