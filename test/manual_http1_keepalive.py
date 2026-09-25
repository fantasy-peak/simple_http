# A manual cross-check of HTTP/1.1 keep-alive with a third-party client.
#
# The automatic suites (xmake run unittest / regression / client) cover this
# without any Python dependency; this one is here for a quick eyeball against a
# running example server, using a client from outside the library:
#
#   pip install requests
#   xmake run server            # plaintext :7788, TLS :7789
#   python3 test/manual_http1_keepalive.py
#
# Both requests should report 200 and reuse the same connection (`Connection:
# keep-alive`), which is what the second response's timing shows.

import requests

session = requests.Session()

url = "http://127.0.0.1:7788/hello"

# 第一个请求
response1 = session.get(url)
print("Response 1:", response1.status_code)

# 第二个请求（复用同一个 TCP 连接）
response2 = session.get(url)
print("Response 2:", response2.status_code)

session.close()