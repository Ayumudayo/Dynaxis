#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import time
import urllib.request
from typing import cast


def _env_list(name: str, default_csv: str) -> list[str]:
    raw = os.getenv(name, default_csv)
    items = [part.strip() for part in raw.split(",")]
    return [item for item in items if item]


METRICS_URLS = _env_list(
    "RUNTIME_TOGGLE_METRICS_URLS",
    "http://127.0.0.1:39091/metrics,http://127.0.0.1:39092/metrics",
)


def fetch_metrics(url: str) -> str:
    with urllib.request.urlopen(url, timeout=5) as response:
        body = cast(bytes, response.read())
        return body.decode("utf-8", errors="replace")


def parse_metric_values(text: str, metric_name: str) -> list[float]:
    values: list[float] = []
    pattern = re.compile(rf"^{re.escape(metric_name)}(?:\{{[^}}]*\}})?\s+(.+)$")
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        match = pattern.match(line.strip())
        if not match:
            continue
        raw = match.group(1).strip()
        try:
            values.append(float(raw))
        except ValueError:
            continue
    return values


def verify_endpoint(
    url: str, expect_chat_hook: int, expect_lua: int
) -> tuple[bool, str]:
    try:
        text = fetch_metrics(url)
    except Exception as exc:  # noqa: BLE001
        return False, f"fetch failed: {exc}"

    hook_values = parse_metric_values(text, "chat_hook_plugins_enabled")
    if not hook_values:
        return False, "missing metric chat_hook_plugins_enabled"
    if any(int(value) != expect_chat_hook for value in hook_values):
        return False, (
            "chat_hook_plugins_enabled mismatch: "
            + ", ".join(str(value) for value in hook_values)
            + f" (expected {expect_chat_hook})"
        )

    lua_values = parse_metric_values(text, "chat_lua_hooks_enabled")
    if not lua_values:
        return False, "missing metric chat_lua_hooks_enabled"
    if any(int(value) != expect_lua for value in lua_values):
        return False, (
            "chat_lua_hooks_enabled mismatch: "
            + ", ".join(str(value) for value in lua_values)
            + f" (expected {expect_lua})"
        )

    return True, "ok"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify runtime toggle gauges from server metrics",
    )
    _ = parser.add_argument(
        "--expect-chat-hook-enabled",
        type=int,
        choices=[0, 1],
        required=True,
        help="expected chat_hook_plugins_enabled value",
    )
    _ = parser.add_argument(
        "--expect-lua-enabled",
        type=int,
        choices=[0, 1],
        required=True,
        help="expected chat_lua_hooks_enabled value",
    )
    _ = parser.add_argument(
        "--wait-timeout-sec",
        type=float,
        default=float(os.getenv("RUNTIME_TOGGLE_WAIT_TIMEOUT_SEC", "45")),
        help="max wait time for endpoints/metrics readiness",
    )
    _ = parser.add_argument(
        "--poll-interval-sec",
        type=float,
        default=float(os.getenv("RUNTIME_TOGGLE_POLL_INTERVAL_SEC", "0.5")),
        help="poll interval while waiting",
    )
    args = parser.parse_args()

    expect_chat_hook_enabled = cast(int, args.expect_chat_hook_enabled)
    expect_lua_enabled = cast(int, args.expect_lua_enabled)
    wait_timeout_sec = cast(float, args.wait_timeout_sec)
    poll_interval_sec = cast(float, args.poll_interval_sec)

    deadline = time.time() + wait_timeout_sec
    last_errors: dict[str, str] = {url: "not checked" for url in METRICS_URLS}

    while time.time() < deadline:
        all_ok = True
        for url in METRICS_URLS:
            ok, reason = verify_endpoint(
                url,
                expect_chat_hook_enabled,
                expect_lua_enabled,
            )
            if not ok:
                all_ok = False
                last_errors[url] = reason

        if all_ok:
            print(
                "PASS: runtime toggles match expected values "
                + f"(chat_hook={expect_chat_hook_enabled}, lua={expect_lua_enabled})"
            )
            return 0

        time.sleep(poll_interval_sec)

    print("FAIL: runtime toggle metrics did not converge before timeout")
    for url in METRICS_URLS:
        print(f"- {url}: {last_errors.get(url, 'unknown')}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
