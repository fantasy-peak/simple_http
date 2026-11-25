// quic-go HTTP/3 server for the 0-RTT differential. 0-RTT is an explicit
// server policy in quic-go, so it is enabled here just as on the C++ side.
package main

import (
	"crypto/tls"
	"fmt"
	"log"
	"net/http"
	"os"

	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
)

func main() {
	port := "7930"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	enable0RTT := true
	if len(os.Args) > 2 {
		enable0RTT = os.Args[2] != "0"
	}
	cert, err := tls.LoadX509KeyPair("./test/tls_certificates/server_cert.pem",
		"./test/tls_certificates/server_key.pem")
	if err != nil {
		log.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/0rtt", func(w http.ResponseWriter, r *http.Request) {
		early := r.TLS != nil && !r.TLS.HandshakeComplete
		if early {
			fmt.Fprint(w, "early")
		} else {
			fmt.Fprint(w, "normal")
		}
	})
	srv := &http3.Server{
		Addr:       "127.0.0.1:" + port,
		TLSConfig:  &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"h3"}},
		QUICConfig: &quic.Config{Allow0RTT: enable0RTT},
		Handler:    mux,
	}
	fmt.Println("ready")
	log.Fatal(srv.ListenAndServe())
}
