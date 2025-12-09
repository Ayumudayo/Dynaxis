import socket
import time

def connect():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(('127.0.0.1', 36000))
        # Send anonymous token (legacy format)
        s.send(b"anonymous:token")
        s.close()
    except Exception as e:
        print(f"Connection failed: {e}")

for i in range(20):
    connect()
    time.sleep(0.1)
