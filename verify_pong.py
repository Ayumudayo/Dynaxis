import socket
import struct
import time
import sys

HOST = '127.0.0.1'
PORT = 6000 # Gateway port

def verify_pong():
    print(f"Connecting to {HOST}:{PORT}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, PORT))
    except Exception as e:
        print(f"Failed to connect: {e}")
        return

    # Construct MSG_PONG frame
    # Header: length(2), msg_id(2), flags(2), seq(2), timestamp(4)
    msg_id = 0x0003 # MSG_PONG
    length = 0
    flags = 0
    seq = 1
    timestamp = 0
    
    header = struct.pack('>HHHH', length, msg_id, flags, seq) + struct.pack('>I', timestamp)
    
    print("Sending MSG_PONG...")
    sock.sendall(header)
    
    # Wait for response
    sock.settimeout(2.0)
    try:
        data = sock.recv(1024)
        if not data:
            print("Connection closed by server (unexpected).")
            # This might happen if Gateway rejects invalid protocol?
            # But MSG_PONG is valid frame.
        else:
            print(f"Received {len(data)} bytes: {data.hex()}")
            # Parse header
            if len(data) >= 12:
                r_len, r_msg_id, r_flags, r_seq = struct.unpack('>HHHH', data[:8])
                print(f"MsgID: {r_msg_id:#06x}")
                if r_msg_id == 0x0000: # MSG_ERR
                    # Read error code
                    err_code = struct.unpack('>H', data[12:14])[0]
                    print(f"Error Code: {err_code}")
                    if err_code == 3:
                        print("FAIL: Received UNKNOWN_MSG_ID (3) for MSG_PONG")
                        sys.exit(1)
            
            print("PASS: Did not receive ERR 3")
            
    except socket.timeout:
        print("PASS: No response received (expected for PONG)")

    sock.close()

if __name__ == "__main__":
    verify_pong()
