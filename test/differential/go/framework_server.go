// Go reference server for the framework-layer differential test.
//
// It implements the same observable behavior as test/framework_server.cpp using
// only the Go standard library: typed path/query/JSON/form parsing, application
// state, sessions, CSRF, HS256 JWT, host routing and catch-all paths.
//
//	go run test/differential/go/framework_server.go 7799

package main

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/hmac"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"math/big"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	jwtSecret   = "secret"
	jwtIssuer   = "iss"
	jwtAudience = "aud"
	jwtLeeway   = 60
)

type createUser struct {
	Name string `json:"name"`
	Age  int    `json:"age"`
}

type jwtHeader struct {
	Alg string `json:"alg"`
	Typ string `json:"typ"`
}

type jwtPayload struct {
	Sub   string   `json:"sub"`
	Scope string   `json:"scope"`
	Roles []string `json:"roles"`
	Iss   string   `json:"iss"`
	Aud   string   `json:"aud"`
	Exp   int64    `json:"exp"`
	Nbf   int64    `json:"nbf"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Details any    `json:"details,omitempty"`
}

type sessionStore struct {
	mu   sync.Mutex
	data map[string]map[string]string
}

func newSessionStore() *sessionStore {
	return &sessionStore{data: make(map[string]map[string]string)}
}

func (s *sessionStore) get(id, key string) (string, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	values, ok := s.data[id]
	if !ok {
		return "", false
	}
	value, ok := values[key]
	return value, ok
}

func (s *sessionStore) set(id, key, value string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.data[id] == nil {
		s.data[id] = make(map[string]string)
	}
	s.data[id][key] = value
}

func (s *sessionStore) remove(id string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.data, id)
}

func randomToken(bytes int) string {
	buf := make([]byte, bytes)
	if _, err := rand.Read(buf); err != nil {
		panic(err)
	}
	return base64.RawURLEncoding.EncodeToString(buf)
}

func writeText(w http.ResponseWriter, status int, body string) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(status)
	_, _ = w.Write([]byte(body))
}

func writeError(w http.ResponseWriter, status int, code, message string, details any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(errorBody{Code: code, Message: message, Details: details})
}

func writeUnauthorized(w http.ResponseWriter) {
	w.Header().Set("WWW-Authenticate", `Bearer error="invalid_token"`)
	writeError(w, http.StatusUnauthorized, "unauthorized", "invalid bearer token", nil)
}

func writeForbidden(w http.ResponseWriter, code string) {
	writeError(w, http.StatusForbidden, code, "access denied", nil)
}

func normalizeHost(host string) string {
	host = strings.ToLower(host)
	return strings.TrimRight(host, ".")
}

func withoutPort(host string) string {
	if strings.HasPrefix(host, "[") {
		if end := strings.Index(host, "]"); end >= 0 {
			return host[:end+1]
		}
		return host
	}
	if strings.Count(host, ":") == 1 {
		if colon := strings.Index(host, ":"); colon >= 0 {
			return host[:colon]
		}
	}
	return host
}

func whoHandler(w http.ResponseWriter, r *http.Request) {
	host := normalizeHost(r.Host)
	switch {
	case host == "api.example.com:8080":
		writeText(w, http.StatusOK, "port")
	case withoutPort(host) == "api.example.com":
		writeText(w, http.StatusOK, "api")
	case strings.HasSuffix(withoutPort(host), ".example.com"):
		writeText(w, http.StatusOK, "sub")
	default:
		writeText(w, http.StatusOK, "root")
	}
}

func filesHandler(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/files/")
	switch {
	case r.URL.Path == "/files/static":
		writeText(w, http.StatusOK, "static")
	case rest == "":
		writeText(w, http.StatusOK, "rest=")
	case strings.Contains(rest, "/"):
		writeText(w, http.StatusOK, "rest="+rest)
	default:
		writeText(w, http.StatusOK, "id="+rest)
	}
}

func loadSession(w http.ResponseWriter, r *http.Request) string {
	if cookie, err := r.Cookie("sid"); err == nil && cookie.Value != "" {
		http.SetCookie(w, &http.Cookie{
			Name: "sid", Value: cookie.Value, Path: "/", HttpOnly: true,
			SameSite: http.SameSiteLaxMode, MaxAge: 86400,
		})
		return cookie.Value
	}
	id := randomToken(32)
	http.SetCookie(w, &http.Cookie{
		Name: "sid", Value: id, Path: "/", HttpOnly: true,
		SameSite: http.SameSiteLaxMode, MaxAge: 86400,
	})
	return id
}

func csrfToken(w http.ResponseWriter, r *http.Request) string {
	if cookie, err := r.Cookie("csrf"); err == nil && cookie.Value != "" {
		return cookie.Value
	}
	token := randomToken(32)
	http.SetCookie(w, &http.Cookie{
		Name: "csrf", Value: token, Path: "/", SameSite: http.SameSiteLaxMode,
	})
	return token
}

type jwtVerifier struct {
	alg       string
	secret    []byte
	publicKey any
}

func loadPublicKey(path string) (any, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return nil, fmt.Errorf("no PEM block in %s", path)
	}
	return x509.ParsePKIXPublicKey(block.Bytes)
}

func (v jwtVerifier) verify(input string, signature []byte) bool {
	digest := sha256.Sum256([]byte(input))
	switch v.alg {
	case "HS256":
		mac := hmac.New(sha256.New, v.secret)
		_, _ = mac.Write([]byte(input))
		return subtle.ConstantTimeCompare(mac.Sum(nil), signature) == 1
	case "RS256":
		key, ok := v.publicKey.(*rsa.PublicKey)
		return ok && rsa.VerifyPKCS1v15(key, crypto.SHA256, digest[:], signature) == nil
	case "ES256":
		key, ok := v.publicKey.(*ecdsa.PublicKey)
		if !ok || len(signature) != 64 {
			return false
		}
		r := new(big.Int).SetBytes(signature[:32])
		s := new(big.Int).SetBytes(signature[32:])
		return ecdsa.Verify(key, digest[:], r, s)
	default:
		return false
	}
}

func jwtAuthenticate(w http.ResponseWriter, r *http.Request, verifier jwtVerifier) (string, []string, bool) {
	header := r.Header.Get("Authorization")
	if header == "" {
		writeUnauthorized(w)
		return "", nil, false
	}
	const prefix = "Bearer "
	if len(header) <= len(prefix) || !strings.EqualFold(header[:len(prefix)], prefix) {
		writeUnauthorized(w)
		return "", nil, false
	}
	token := header[len(prefix):]
	parts := strings.Split(token, ".")
	if len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
		writeUnauthorized(w)
		return "", nil, false
	}

	headerBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		writeUnauthorized(w)
		return "", nil, false
	}
	var headerJSON jwtHeader
	if json.Unmarshal(headerBytes, &headerJSON) != nil || headerJSON.Alg != verifier.alg {
		writeUnauthorized(w)
		return "", nil, false
	}

	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil || !verifier.verify(parts[0]+"."+parts[1], signature) {
		writeUnauthorized(w)
		return "", nil, false
	}

	payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		writeUnauthorized(w)
		return "", nil, false
	}
	var payload jwtPayload
	if json.Unmarshal(payloadBytes, &payload) != nil {
		writeUnauthorized(w)
		return "", nil, false
	}
	now := time.Now().Unix()
	if payload.Exp == 0 || now > payload.Exp+jwtLeeway {
		writeUnauthorized(w)
		return "", nil, false
	}
	if payload.Nbf != 0 && now+jwtLeeway < payload.Nbf {
		writeUnauthorized(w)
		return "", nil, false
	}
	if payload.Iss != jwtIssuer || payload.Aud != jwtAudience {
		writeUnauthorized(w)
		return "", nil, false
	}
	return payload.Sub, strings.Fields(payload.Scope), true
}

func main() {
	port := "7799"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	sessions := newSessionStore()
	mux := http.NewServeMux()

	mux.HandleFunc("GET /state", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, "from-state")
	})

	mux.HandleFunc("GET /users/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.Atoi(r.PathValue("id"))
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid_path", "invalid or missing path parameters", nil)
			return
		}
		writeText(w, http.StatusOK, strconv.Itoa(id)+":from-state")
	})

	mux.HandleFunc("GET /list", func(w http.ResponseWriter, r *http.Request) {
		page, err := strconv.Atoi(r.URL.Query().Get("page"))
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid_query", "invalid or missing query parameters", nil)
			return
		}
		writeText(w, http.StatusOK, r.URL.Query().Get("q")+":"+strconv.Itoa(page))
	})

	mux.HandleFunc("POST /json", func(w http.ResponseWriter, r *http.Request) {
		var in createUser
		if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_json", "malformed JSON request body", nil)
			return
		}
		writeText(w, http.StatusCreated, in.Name+":"+strconv.Itoa(in.Age))
	})

	mux.HandleFunc("POST /valid", func(w http.ResponseWriter, r *http.Request) {
		var in createUser
		if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_json", "malformed JSON request body", nil)
			return
		}
		details := make([]map[string]string, 0, 2)
		if in.Name == "" {
			details = append(details, map[string]string{"field": "name", "code": "required"})
		}
		if in.Age < 1 || in.Age > 150 {
			details = append(details, map[string]string{"field": "age", "code": "range"})
		}
		if len(details) != 0 {
			writeError(w, http.StatusUnprocessableEntity, "validation_failed", "request validation failed", details)
			return
		}
		writeText(w, http.StatusCreated, in.Name)
	})

	mux.HandleFunc("POST /form", func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseForm(); err != nil {
			writeText(w, http.StatusBadRequest, "invalid form")
			return
		}
		age, err := strconv.Atoi(r.PostForm.Get("age"))
		if err != nil {
			writeText(w, http.StatusBadRequest, "invalid form")
			return
		}
		writeText(w, http.StatusCreated, r.PostForm.Get("name")+":"+strconv.Itoa(age))
	})

	mux.HandleFunc("GET /header", func(w http.ResponseWriter, r *http.Request) {
		rawPage := r.Header.Get("X-Page")
		if rawPage == "" {
			writeError(w, http.StatusBadRequest, "missing_header", "missing required header", "x-page")
			return
		}
		page, err := strconv.Atoi(rawPage)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid_header", "invalid header value", "x-page")
			return
		}
		cookie, err := r.Cookie("sid")
		if err != nil {
			writeError(w, http.StatusBadRequest, "missing_cookie", "missing required cookie", "sid")
			return
		}
		writeText(w, http.StatusOK, strconv.Itoa(page)+":"+cookie.Value)
	})

	mux.HandleFunc("GET /files/", filesHandler)
	mux.HandleFunc("GET /who", whoHandler)

	mux.HandleFunc("POST /session/set", func(w http.ResponseWriter, r *http.Request) {
		id := loadSession(w, r)
		sessions.set(id, "uid", "42")
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("GET /session/get", func(w http.ResponseWriter, r *http.Request) {
		id := loadSession(w, r)
		if uid, ok := sessions.get(id, "uid"); ok {
			writeText(w, http.StatusOK, uid)
		} else {
			writeText(w, http.StatusOK, "none")
		}
	})
	mux.HandleFunc("GET /session/rotate", func(w http.ResponseWriter, r *http.Request) {
		oldID := loadSession(w, r)
		sessions.remove(oldID)
		newID := randomToken(32)
		sessions.set(newID, "uid", "99")
		http.SetCookie(w, &http.Cookie{
			Name: "sid", Value: newID, Path: "/", HttpOnly: true,
			SameSite: http.SameSiteLaxMode, MaxAge: 86400,
		})
		w.WriteHeader(http.StatusNoContent)
	})

	mux.HandleFunc("GET /csrf/token", func(w http.ResponseWriter, r *http.Request) {
		writeText(w, http.StatusOK, csrfToken(w, r))
	})
	mux.HandleFunc("POST /csrf/submit", func(w http.ResponseWriter, r *http.Request) {
		token := csrfToken(w, r)
		supplied := r.Header.Get("X-Csrf-Token")
		if token == "" || subtle.ConstantTimeCompare([]byte(token), []byte(supplied)) != 1 {
			writeText(w, http.StatusForbidden, "CSRF token invalid")
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("POST /csrf/exempt", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})

	registerJWT := func(prefix string, verifier jwtVerifier) {
		mux.HandleFunc("GET "+prefix+"/me", func(w http.ResponseWriter, r *http.Request) {
			sub, scopes, ok := jwtAuthenticate(w, r, verifier)
			if !ok {
				return
			}
			writeText(w, http.StatusOK, sub+":"+strings.Join(scopes, ","))
		})
		mux.HandleFunc("GET "+prefix+"/write", func(w http.ResponseWriter, r *http.Request) {
			_, scopes, ok := jwtAuthenticate(w, r, verifier)
			if !ok {
				return
			}
			for _, scope := range scopes {
				if scope == "write" {
					w.WriteHeader(http.StatusNoContent)
					return
				}
			}
			writeForbidden(w, "forbidden")
		})
	}

	registerJWT("/jwt", jwtVerifier{alg: "HS256", secret: []byte(jwtSecret)})
	if len(os.Args) > 2 {
		key, err := loadPublicKey(os.Args[2])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		registerJWT("/jwt/rs256", jwtVerifier{alg: "RS256", publicKey: key})
	}
	if len(os.Args) > 3 {
		key, err := loadPublicKey(os.Args[3])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		registerJWT("/jwt/es256", jwtVerifier{alg: "ES256", publicKey: key})
	}

	addr := "127.0.0.1:" + port
	fmt.Printf("go reference server on %s\n", addr)
	server := &http.Server{Addr: addr, Handler: mux}
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
