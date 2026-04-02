import websocket # pip install websocket-client
import ssl

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.load_verify_locations("./tls_certificates/ca_cert.pem")
context.load_cert_chain("./tls_certificates/client_cert.pem", "./tls_certificates/client_key.pem")
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE

ws = websocket.create_connection(
    "wss://127.0.0.1:7788/wss", 
    sslopt={"context": context},
    header=["Host: SimpleHttpServer"] # 对应你 curl 的 resolve 逻辑
)

ws.send("Hello Server")
print("Received:", ws.recv())
ws.close()