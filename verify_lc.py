import socket
import threading
import time

def connect_and_hold(client_id, batch_id):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(('127.0.0.1', 36000))
        # Keep connection open for longer than the test
        s.send(f"anonymous:{batch_id}-{client_id}".encode())
        time.sleep(30) 
        s.close()
    except Exception as e:
        print(f"Client {batch_id}-{client_id} failed: {e}")

threads = []
print("Starting LC verification with batches...")

for batch in range(4):
    print(f"Starting Batch {batch} (5 connections)...")
    for i in range(5):
        t = threading.Thread(target=connect_and_hold, args=(i, batch))
        threads.append(t)
        t.start()
        time.sleep(0.1) # Small stagger
    
    print("Waiting 6 seconds for heartbeat update...")
    time.sleep(6) 

for t in threads:
    t.join()
print("Done.")
