import socket
import struct
import time
import threading

HOST = '127.0.0.1'
PORT = 6000 # Gateway port

def connect_client(user_id):
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, PORT))
        
        # MSG_LOGIN_REQ = 0x0010
        msg_id = 0x0010
        flags = 0
        seq = 1
        timestamp = 0
        
        # Use unique username for each client to test load balancing
        username = f"user{user_id}".encode('utf-8')
        token = b""
        
        # Payload: LP string username, LP string token
        payload = struct.pack('>H', len(username)) + username
        payload += struct.pack('>H', len(token)) + token
        
        print(f"User {user_id} connecting with username: user{user_id}")
        
        length = len(payload)
        header = struct.pack('>HHHH', length, msg_id, flags, seq) + struct.pack('>I', timestamp)
        
        sock.sendall(header + payload)
        
        # Keep connection open for a bit
        time.sleep(5)
        sock.close()
        print(f"User {user_id} connected and closed.")
    except Exception as e:
        print(f"User {user_id} failed: {e}")

threads = []
for i in range(10):
    t = threading.Thread(target=connect_client, args=(i,))
    threads.append(t)
    t.start()
    time.sleep(0.5) # Stagger connections

for t in threads:
    t.join()

print("All clients finished.")
