"""A metrics bind failure must unwind live workers without terminate/hang."""
import os
import socket
import subprocess
import sys


def main():
    with socket.socket() as occupied, socket.socket() as candidate:
        if sys.platform == "win32":
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        occupied.bind(("0.0.0.0", 0))
        occupied.listen()
        candidate.bind(("127.0.0.1", 0))
        port = candidate.getsockname()[1]
        candidate.close()
        env = os.environ.copy()
        env.update(PORT=str(port), METRICS_PORT=str(occupied.getsockname()[1]),
                   DB_URI="", REDIS_URI="", CHAT_AUTH_MODE="development")
        result = subprocess.run([sys.argv[1]], env=env, capture_output=True, timeout=10)
        output = result.stdout + result.stderr
        assert result.returncode == 1, f"unexpected startup exit {result.returncode}"
        assert b"SERVER_FATAL" in output, "failure did not reach bootstrap error handling"
        assert b"I/O threads started" in output, "failure did not exercise live-thread unwind"
    print("PASS: live-thread startup failure stops, joins and exits with handled error")


if __name__ == "__main__":
    main()
