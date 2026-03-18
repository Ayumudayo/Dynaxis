#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
import re
from datetime import datetime, timezone


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize same-run Windows sccache PoC timings and stats into one JSON artifact."
    )
    parser.add_argument("--output", required=True, help="Output JSON path.")
    parser.add_argument("--target", required=True, help="Build target under test.")
    parser.add_argument("--double-build", required=True, help="Whether pass #2 was requested.")
    parser.add_argument(
        "--reset-sccache-before-measurement",
        required=True,
        help="Whether sccache contents were cleared before pass #1.",
    )
    parser.add_argument("--baseline-first-sec", type=float, default=None)
    parser.add_argument("--baseline-second-sec", type=float, default=None)
    parser.add_argument("--sccache-first-sec", type=float, default=None)
    parser.add_argument("--sccache-second-sec", type=float, default=None)
    parser.add_argument("--pass1-stats-path", default="")
    parser.add_argument("--pass2-stats-path", default="")
    parser.add_argument("--conan-cache-hit", default="")
    parser.add_argument("--conan-cache-matched-key", default="")
    parser.add_argument("--sccache-cache-hit", default="")
    parser.add_argument("--sccache-cache-matched-key", default="")
    parser.add_argument("--run-id", default="")
    parser.add_argument("--run-attempt", default="")
    parser.add_argument("--runner-os", default="")
    parser.add_argument("--step-summary", default="")
    return parser.parse_args()


def parse_stats(path_value: str) -> dict[str, object] | None:
    if not path_value:
        return None
    path = pathlib.Path(path_value)
    if not path.exists():
        return None
    raw = path.read_text(encoding="utf-8", errors="replace")
    metrics: dict[str, object] = {
        "raw_text_path": str(path).replace("\\", "/"),
    }
    patterns = {
        "compile_requests": r"Compile requests\s+([0-9]+)",
        "compile_requests_executed": r"Compile requests executed\s+([0-9]+)",
        "cache_hits": r"Cache hits\s+([0-9]+)",
        "cache_misses": r"Cache misses\s+([0-9]+)",
        "cache_timeouts": r"Cache timeouts\s+([0-9]+)",
        "cache_write_errors": r"Cache write errors\s+([0-9]+)",
        "failed_compilations": r"Failed compilations\s+([0-9]+)",
        "non_cacheable_compilations": r"Non-cacheable compilations\s+([0-9]+)",
        "cache_hit_rate_pct": r"Cache hits? rate\s+([0-9.]+)\s*%",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, raw)
        if not match:
            continue
        if key == "cache_hit_rate_pct":
            metrics[key] = float(match.group(1))
        else:
            metrics[key] = int(match.group(1))
    if "cache_hit_rate_pct" not in metrics:
        hits = metrics.get("cache_hits")
        misses = metrics.get("cache_misses")
        if isinstance(hits, int) and isinstance(misses, int) and (hits + misses) > 0:
            metrics["cache_hit_rate_pct"] = round((hits / (hits + misses)) * 100.0, 2)
    return metrics


def delta(a: float | None, b: float | None) -> float | None:
    if a is None or b is None:
        return None
    return round(a - b, 2)


def speedup(baseline: float | None, candidate: float | None) -> float | None:
    if baseline is None or candidate is None or baseline == 0:
        return None
    return round(((baseline - candidate) / baseline) * 100.0, 2)


def append_summary(path_value: str, payload: dict[str, object], pass2_stats: dict[str, object] | None) -> None:
    if not path_value:
        return
    path = pathlib.Path(path_value)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "### sccache PoC comparison artifact",
        f"- summary_json: {pathlib.Path(str(payload['artifact_path'])).as_posix()}",
    ]
    comparison = payload["comparison"]
    if isinstance(comparison, dict):
        pass1_speedup = comparison.get("pass_1_speedup_pct")
        pass2_speedup = comparison.get("pass_2_speedup_pct")
        if pass1_speedup is not None:
            lines.append(f"- pass_1_speedup_pct: {pass1_speedup}")
        if pass2_speedup is not None:
            lines.append(f"- pass_2_speedup_pct: {pass2_speedup}")
    if pass2_stats and pass2_stats.get("cache_hit_rate_pct") is not None:
        lines.append(f"- pass_2_cache_hit_rate_pct: {pass2_stats['cache_hit_rate_pct']}")
    with path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    pass1_stats = parse_stats(args.pass1_stats_path)
    pass2_stats = parse_stats(args.pass2_stats_path)
    output_path = pathlib.Path(args.output)
    payload: dict[str, object] = {
        "workflow": "windows-sccache-poc",
        "job": "windows-sccache-poc",
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "runner_os": args.runner_os,
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "target": args.target,
        "double_build": args.double_build,
        "reset_sccache_before_measurement": args.reset_sccache_before_measurement,
        "cache_restore": {
            "conan_cache_hit": args.conan_cache_hit,
            "conan_cache_matched_key": args.conan_cache_matched_key,
            "sccache_cache_hit": args.sccache_cache_hit,
            "sccache_cache_matched_key": args.sccache_cache_matched_key,
        },
        "without_sccache": {
            "build_dir": "build-windows-nosccache-conan",
            "pass_1_sec": args.baseline_first_sec,
            "pass_2_sec": args.baseline_second_sec,
        },
        "with_sccache": {
            "build_dir": "build-windows-sccache-conan",
            "pass_1_sec": args.sccache_first_sec,
            "pass_2_sec": args.sccache_second_sec,
            "pass_1_stats": pass1_stats,
            "pass_2_stats": pass2_stats,
        },
        "comparison": {
            "pass_1_delta_sec": delta(args.baseline_first_sec, args.sccache_first_sec),
            "pass_1_speedup_pct": speedup(args.baseline_first_sec, args.sccache_first_sec),
            "pass_2_delta_sec": delta(args.baseline_second_sec, args.sccache_second_sec),
            "pass_2_speedup_pct": speedup(args.baseline_second_sec, args.sccache_second_sec),
        },
        "notes": [
            "pass_1 is only cold within the current run when reset_sccache_before_measurement=true",
            "cache restore telemetry is recorded separately so CI warmth and same-run cold/warm measurement are not conflated",
        ],
        "artifact_path": str(output_path).replace("\\", "/"),
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    append_summary(args.step_summary, payload, pass2_stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
