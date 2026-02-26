# pip install httpx
# pip install h2
# pip install urllib3
import httpx
import asyncio
import os
import datetime
import urllib3

# Disable SSL certificate verification warnings for cleaner logs
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# --- Certificate Root Configuration ---
CERT_ROOT = "./tls_certificates"

CA_CERT = os.path.join(CERT_ROOT, "ca_cert.pem")
CLIENT_CERT = os.path.join(CERT_ROOT, "client_cert.pem")
CLIENT_KEY = os.path.join(CERT_ROOT, "client_key.pem")

async def frame_generator():
    """Simulate streaming data frames generation"""
    for i in range(5):
        timestamp = datetime.datetime.now().strftime("%H:%M:%S")
        data = f"Data-Frame-{i} at {timestamp}\n".encode()
        
        # Log sending event
        print(f"[{timestamp}] [SENDING] Preparing frame {i}: {data.decode().strip()}")
        
        yield data
        await asyncio.sleep(1)
    
    print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] [FINISHED] All data frames sent to buffer")

async def stream_request():
    # Check if certificate files exist
    for f in [CA_CERT, CLIENT_CERT, CLIENT_KEY]:
        if not os.path.exists(f):
            print(f"Error: File not found {f}")
            return

    client_auth = (CLIENT_CERT, CLIENT_KEY)

    # Note: verify=False skips SSL certificate verification errors
    async with httpx.AsyncClient(
        http2=True, 
        verify=False, 
        cert=client_auth,
        timeout=None
    ) as client:
        try:
            print("Establishing HTTP/2 connection...")
            async with client.stream(
                "POST", 
                "https://127.0.0.1:7788/hello", 
                content=frame_generator(),
                headers={"Host": "SimpleHttpServer"}
            ) as response:
                # Log successful connection details
                print(f"--- Connection Established ---")
                print(f"Status Code: {response.status_code} | Protocol: {response.http_version}")
                
                print("Waiting for server streaming response...")
                async for line in response.aiter_raw():
                    recv_time = datetime.datetime.now().strftime("%H:%M:%S")
                    print(f"[{recv_time}] [SERVER RESPONSE]: {line}")
                    
        except Exception as e:
            print(f"Request failed: {e}")

if __name__ == "__main__":
    asyncio.run(stream_request())
