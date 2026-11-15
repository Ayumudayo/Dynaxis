import math
import os
import socket
import struct
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed


HOST = os.getenv("SOAK_HOST", "127.0.0.1")
PORT = int(os.getenv("SOAK_PORT", "30001"))
METRICS_URL = os.getenv("SOAK_METRICS_URL", "http://127.0.0.1:39091/metrics")

WORKERS = int(os.getenv("SOAK_WORKERS", "6"))
REQUESTS_PER_WORKER = int(os.getenv("SOAK_REQUESTS_PER_WORKER", "60"))

P95_MAX_MS = float(os.getenv("PERF_P95_MAX_MS", "250"))
P99_MAX_MS = float(os.getenv("PERF_P99_MAX_MS", "500"))
MIN_THROUGHPUT_RPS = float(os.getenv("PERF_MIN_THROUGHPUT_RPS", "8"))

MAX_ERROR_RATE = float(os.getenv("SOAK_MAX_ERROR_RATE", "0"))

MAX_JOB_QUEUE_DEPTH = int(os.getenv("SOAK_MAX_JOB_QUEUE_DEPTH", "8192"))
MAX_ACTIVE_SESSIONS_AFTER = int(os.getenv("SOAK_MAX_ACTIVE_SESSIONS_AFTER", "64"))

HEADER_BYTES = 14
MSG_ERR = 0x0004
MSG_HELLO = 0x0001


def _read_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    total = 0
    while total < n:
        chunk = sock.recv(n - total)
        if not chunk:
            raise RuntimeError("socket closed while reading")
        chunks.append(chunk)
        total += len(chunk)
    return b"".join(chunks)


def _read_frame(sock: socket.socket) -> tuple[int, bytes]:
    header = _read_exact(sock, HEADER_BYTES)
    payload_len, msg_id, _flags, _seq, _timestamp = struct.unpack(">HHHII", header)
    payload = _read_exact(sock, payload_len) if payload_len > 0 else b""
    return msg_id, payload


def _single_connect_roundtrip(_worker_id: int, _idx: int) -> float:
    started = time.perf_counter()
    with socket.create_connection((HOST, PORT), timeout=3.0) as sock:
        sock.settimeout(3.0)
        msg_id, payload = _read_frame(sock)
        if msg_id != MSG_HELLO:
            if msg_id == MSG_ERR:
                raise RuntimeError("received MSG_ERR on connect path")
            raise RuntimeError(f"unexpected first frame msg_id={msg_id}")
        if len(payload) != 12 or int.from_bytes(payload[:2], "big") != 1:
            raise RuntimeError("invalid HELLO payload")

    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return elapsed_ms


def _percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = int(round((p / 100.0) * (len(ordered) - 1)))
    return ordered[rank]


def _read_metric_value(text: str, metric: str) -> float | None:
    for line in text.splitlines():
        if line.startswith(metric + " "):
            try:
                return float(line.split(" ", 1)[1].strip())
            except Exception:
                return None
    return None


def _fetch_metrics() -> str:
    with urllib.request.urlopen(METRICS_URL, timeout=5) as response:
        return response.read().decode("utf-8", errors="replace")


def main() -> int:
    if WORKERS <= 0 or REQUESTS_PER_WORKER <= 0 or not math.isfinite(MAX_ERROR_RATE) or not 0 <= MAX_ERROR_RATE <= 1:
        print("FAIL: invalid workload or SOAK_MAX_ERROR_RATE (expected 0..1)")
        return 1
    started = time.perf_counter()
    latencies: list[float] = []
    errors = 0

    with ThreadPoolExecutor(max_workers=WORKERS) as executor:
        futures = []
        for worker_id in range(WORKERS):
            for idx in range(REQUESTS_PER_WORKER):
                futures.append(executor.submit(_single_connect_roundtrip, worker_id, idx))

        for future in as_completed(futures):
            try:
                latencies.append(future.result())
            except Exception as exc:
                errors += 1
                print(f"WARN: connect roundtrip failed: {exc}")

    elapsed = max(time.perf_counter() - started, 0.001)
    total = WORKERS * REQUESTS_PER_WORKER
    success = len(latencies)
    throughput = success / elapsed

    p95 = _percentile(latencies, 95.0)
    p99 = _percentile(latencies, 99.0)

    print(
        "soak_perf_summary "
        f"total={total} success={success} errors={errors} "
        f"elapsed_sec={elapsed:.2f} throughput_rps={throughput:.2f} "
        f"p95_ms={p95:.2f} p99_ms={p99:.2f}"
    )

    error_rate = errors / total
    if error_rate > MAX_ERROR_RATE:
        print(f"FAIL: request error rate ({error_rate:.3f} > {MAX_ERROR_RATE:.3f})")
        return 1

    if success == 0:
        print("FAIL: no successful roundtrips")
        return 1

    if p95 > P95_MAX_MS:
        print(f"FAIL: p95 regression ({p95:.2f}ms > {P95_MAX_MS:.2f}ms)")
        return 1
    if p99 > P99_MAX_MS:
        print(f"FAIL: p99 regression ({p99:.2f}ms > {P99_MAX_MS:.2f}ms)")
        return 1
    if throughput < MIN_THROUGHPUT_RPS:
        print(f"FAIL: throughput regression ({throughput:.2f}rps < {MIN_THROUGHPUT_RPS:.2f}rps)")
        return 1

    time.sleep(2.0)
    metrics = _fetch_metrics()
    job_depth = _read_metric_value(metrics, "chat_job_queue_depth")
    active_sessions = _read_metric_value(metrics, "chat_session_active")

    if job_depth is None or active_sessions is None:
        print("FAIL: required metrics missing (chat_job_queue_depth/chat_session_active)")
        return 1

    if job_depth > MAX_JOB_QUEUE_DEPTH:
        print(f"FAIL: job queue not bounded ({job_depth} > {MAX_JOB_QUEUE_DEPTH})")
        return 1
    if active_sessions > MAX_ACTIVE_SESSIONS_AFTER:
        print(f"FAIL: active sessions remain high after soak ({active_sessions} > {MAX_ACTIVE_SESSIONS_AFTER})")
        return 1

    print("PASS: soak/performance gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
