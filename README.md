# simple_http

```
curl -v --http2-prior-knowledge http://localhost:6666/hello\?key1\=value1\&key2\=value2
curl -v --http2-prior-knowledge http://localhost:6666/hello -d "aaaa"

curl -v --http2 http://localhost:6666/hello -d "aaaa" -k
nghttp --upgrade -v http://127.0.0.1:6666/hello
nghttp --upgrade -v http://nghttp2.org 
h2load -n 60000 -c 1000 -m 200 -H 'Content-Type: application/json' --data=b.txt http://localhost:6666/hello
```