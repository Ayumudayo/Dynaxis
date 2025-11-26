import subprocess
import time
import os
import sys

# Path to the executable
exe_path = os.path.join("build-windows", "devclient", "Debug", "dev_chat_cli.exe")

if not os.path.exists(exe_path):
    print(f"Error: Executable not found at {exe_path}")
    # Try looking in other common locations
    exe_path = os.path.join("build-windows", "Debug", "dev_chat_cli.exe")
    if not os.path.exists(exe_path):
        print(f"Error: Executable not found at {exe_path} either.")
        sys.exit(1)

print(f"Starting {exe_path}...")
proc = subprocess.Popen(
    [exe_path],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    cwd=os.getcwd() # Run in current directory so log file is here
)

try:
    print("Waiting for startup...")
    time.sleep(3)

    print("Logging in...")
    proc.stdin.write("/login verifier\n")
    proc.stdin.flush()
    time.sleep(1)

    print("Joining lobby...")
    proc.stdin.write("/join lobby\n")
    proc.stdin.flush()
    time.sleep(1)

    print("Sending chat message...")
    proc.stdin.write("/chat hello_verification_msg\n")
    proc.stdin.flush()
    time.sleep(2)

    print("Quitting...")
    proc.stdin.write("/quit\n")
    proc.stdin.flush()
    
    proc.wait(timeout=5)
except Exception as e:
    print(f"Exception: {e}")
    proc.kill()

stdout, stderr = proc.communicate()
print("STDOUT:", stdout)
print("STDERR:", stderr)

print("Done.")
