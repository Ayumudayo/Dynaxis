from __future__ import annotations

import argparse
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from capture_phase5_evidence import ATTACH_ENV_FILE
from capture_phase5_evidence import REPO_ROOT
from capture_phase5_evidence import ensure_linux_loadgen_built
from capture_phase5_evidence import invoke_deploy
from capture_phase5_evidence import wait_stack_ready
from session_continuity_common import read_metric_sum


STACK_NETWORK = "dynaxis-stack_dynaxis-stack"
LOADGEN_IMAGE = "dynaxis-base:latest"
STACK_GATEWAY_HOST = "gateway-1"


def default_run_id() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%SZ")


def run_command(command: list[str], *, label: str, log_path: Path) -> str:
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        check=False,
    )
    combined = ""
    if result.stdout:
        combined += result.stdout
    if result.stderr:
        if combined and not combined.endswith("\n"):
            combined += "\n"
        combined += result.stderr
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(combined, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(
            f"{label} failed ({result.returncode}); see {log_path}\ncommand: {' '.join(command)}"
        )
    if result.stdout.strip():
        print(result.stdout.strip())
    return combined


def summarize_loadgen_report(report_path: Path) -> dict[str, object]:
    payload = json.loads(report_path.read_text(encoding="utf-8"))
    return {
        "scenario": payload["scenario"],
        "success_count": payload["success_count"],
        "error_count": payload["error_count"],
        "throughput_rps": round(float(payload["throughput_rps"]), 4),
        "p95_ms": round(float(payload["latency_ms"]["p95"]), 4),
        "p99_ms": round(float(payload["latency_ms"]["p99"]), 4),
        "attach_failures": int(payload["setup"]["attach_failures"]),
        "udp_bind_successes": int(payload["transport"]["udp_bind_successes"]),
        "rudp_attach_successes": int(payload["transport"]["rudp_attach_successes"]),
        "rudp_attach_fallbacks": int(payload["transport"]["rudp_attach_fallbacks"]),
        "report_path": str(report_path.relative_to(REPO_ROOT)),
    }


def build_udp_only_netem_command(interface: str,
                                 delay_ms: float,
                                 jitter_ms: float,
                                 loss_percent: float,
                                 reorder_percent: float,
                                 duplicate_percent: float) -> str:
    netem_parts = [f"delay {delay_ms:.1f}ms {jitter_ms:.1f}ms"]
    if loss_percent > 0:
        netem_parts.append(f"loss {loss_percent:.2f}%")
    if reorder_percent > 0:
        netem_parts.append(f"reorder {reorder_percent:.2f}% 50%")
    if duplicate_percent > 0:
        netem_parts.append(f"duplicate {duplicate_percent:.2f}%")
    netem = " ".join(netem_parts)
    return (
        f"tc qdisc replace dev {interface} root handle 1: prio bands 4 && "
        f"tc qdisc replace dev {interface} parent 1:4 handle 40: netem {netem} && "
        f"tc filter replace dev {interface} protocol ip parent 1:0 prio 4 "
        f"u32 match ip protocol 17 0xff flowid 1:4"
    )


def run_netem_loadgen_capture(scenario_path: Path,
                              report_path: Path,
                              log_path: Path,
                              *,
                              interface: str,
                              delay_ms: float,
                              jitter_ms: float,
                              loss_percent: float,
                              reorder_percent: float,
                              duplicate_percent: float,
                              host: str,
                              port: int,
                              udp_port: int | None) -> dict[str, object]:
    scenario_rel = scenario_path.relative_to(REPO_ROOT).as_posix()
    report_rel = report_path.relative_to(REPO_ROOT).as_posix()
    netem_command = build_udp_only_netem_command(
        interface,
        delay_ms,
        jitter_ms,
        loss_percent,
        reorder_percent,
        duplicate_percent,
    )
    command = [
        "docker",
        "run",
        "--rm",
        "--cap-add",
        "NET_ADMIN",
        "--network",
        STACK_NETWORK,
        "-v",
        f"{REPO_ROOT}:/workspace",
        "-w",
        "/workspace",
        LOADGEN_IMAGE,
        "bash",
        "-lc",
        "apt-get update >/tmp/netem-apt.log && "
        "apt-get install -y --no-install-recommends iproute2 >/tmp/netem-install.log && "
        + netem_command
        + " && "
        + f"./build-linux/stack_loadgen --host {host} --port {port}"
        + (f" --udp-port {udp_port}" if udp_port is not None else "")
        + f" --scenario {scenario_rel} --report {report_rel}",
    ]
    run_command(command, label=f"netem-loadgen:{scenario_path.stem}", log_path=log_path)
    summary = summarize_loadgen_report(report_path)
    print(
        f"{summary['scenario']}: success={summary['success_count']} errors={summary['error_count']} "
        f"throughput_rps={summary['throughput_rps']} p95_ms={summary['p95_ms']}"
    )
    summary["log_path"] = str(log_path.relative_to(REPO_ROOT))
    return summary


def summarize_metrics() -> dict[str, float]:
    return {
        "gateway_udp_loss_estimated_total": read_metric_sum("gateway_udp_loss_estimated_total"),
        "gateway_udp_reorder_drop_total": read_metric_sum("gateway_udp_reorder_drop_total"),
        "gateway_udp_duplicate_drop_total": read_metric_sum("gateway_udp_duplicate_drop_total"),
        "gateway_udp_jitter_ms_last": read_metric_sum("gateway_udp_jitter_ms_last"),
    }


def assert_summary_ok(summary: dict[str, object], *, expect_rudp: bool, min_expected_p95_ms: float) -> None:
    if int(summary["error_count"]) != 0:
        raise AssertionError(f"loadgen reported errors: {summary}")
    if int(summary["attach_failures"]) != 0:
        raise AssertionError(f"loadgen attach failures were non-zero: {summary}")
    if float(summary["throughput_rps"]) <= 0:
        raise AssertionError(f"loadgen throughput did not make forward progress: {summary}")
    if float(summary["p95_ms"]) < min_expected_p95_ms:
        raise AssertionError(
            f"netem rehearsal did not materially move report latency: p95_ms={summary['p95_ms']} "
            f"threshold={min_expected_p95_ms} summary={summary}"
        )
    if expect_rudp:
        if int(summary["rudp_attach_successes"]) <= 0:
            raise AssertionError(f"RUDP attach did not succeed under netem: {summary}")
    else:
        if int(summary["udp_bind_successes"]) <= 0:
            raise AssertionError(f"UDP bind did not succeed under netem: {summary}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Manual ops-only FPS netem rehearsal against the Linux stack path."
    )
    parser.add_argument("--run-id", default=default_run_id())
    parser.add_argument(
        "--scenario",
        choices=("udp-fps", "rudp-fps", "fps-pair"),
        default="fps-pair",
    )
    parser.add_argument("--compose-env-file", default=str(ATTACH_ENV_FILE))
    parser.add_argument("--shaped-interface", default="eth0")
    parser.add_argument("--delay-ms", type=float, default=35.0)
    parser.add_argument("--jitter-ms", type=float, default=12.0)
    parser.add_argument("--loss-percent", type=float, default=1.5)
    parser.add_argument("--reorder-percent", type=float, default=10.0)
    parser.add_argument("--duplicate-percent", type=float, default=0.0)
    parser.add_argument("--stack-ready-timeout-sec", type=float, default=90.0)
    parser.add_argument("--min-expected-jitter-ms", type=float, default=1.0)
    parser.add_argument("--min-expected-report-p95-ms", type=float, default=50.0)
    args = parser.parse_args()

    env_file = Path(args.compose_env_file)
    if not env_file.is_absolute():
        env_file = (REPO_ROOT / env_file).resolve()

    artifact_root = REPO_ROOT / "build" / "phase5-evidence" / args.run_id / "netem"
    loadgen_root = artifact_root / "loadgen"
    artifact_root.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "run_id": args.run_id,
        "scenario": args.scenario,
        "netem": {
            "interface": args.shaped_interface,
            "delay_ms": args.delay_ms,
            "jitter_ms": args.jitter_ms,
            "loss_percent": args.loss_percent,
            "reorder_percent": args.reorder_percent,
            "duplicate_percent": args.duplicate_percent,
            "scope": "udp-only client egress shaping inside the loadgen container",
        },
        "reports": [],
    }

    try:
        invoke_deploy("down", env_file=env_file, detached=False, build=False, log_path=artifact_root / "preclean.log")
        invoke_deploy("up", env_file=env_file, detached=True, build=True, log_path=artifact_root / "up.log")
        wait_stack_ready(args.stack_ready_timeout_sec)
        ensure_linux_loadgen_built(artifact_root / "build-stack-loadgen-linux.log")

        before_metrics = summarize_metrics()
        manifest["metrics_before"] = before_metrics

        if args.scenario in {"udp-fps", "fps-pair"}:
            summary = run_netem_loadgen_capture(
                REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_udp_fps_soak.json",
                REPO_ROOT / "build" / "loadgen" / f"mixed_direct_udp_fps_soak.{args.run_id}.netem.json",
                loadgen_root / "mixed_direct_udp_fps_soak.log",
                interface=args.shaped_interface,
                delay_ms=args.delay_ms,
                jitter_ms=args.jitter_ms,
                loss_percent=args.loss_percent,
                reorder_percent=args.reorder_percent,
                duplicate_percent=args.duplicate_percent,
                host=STACK_GATEWAY_HOST,
                port=6000,
                udp_port=7000,
            )
            assert_summary_ok(summary, expect_rudp=False, min_expected_p95_ms=args.min_expected_report_p95_ms)
            manifest["reports"].append(summary)

        if args.scenario in {"rudp-fps", "fps-pair"}:
            summary = run_netem_loadgen_capture(
                REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_fps_soak.json",
                REPO_ROOT / "build" / "loadgen" / f"mixed_direct_rudp_fps_soak.{args.run_id}.netem.json",
                loadgen_root / "mixed_direct_rudp_fps_soak.log",
                interface=args.shaped_interface,
                delay_ms=args.delay_ms,
                jitter_ms=args.jitter_ms,
                loss_percent=args.loss_percent,
                reorder_percent=args.reorder_percent,
                duplicate_percent=args.duplicate_percent,
                host=STACK_GATEWAY_HOST,
                port=6000,
                udp_port=7000,
            )
            assert_summary_ok(summary, expect_rudp=True, min_expected_p95_ms=args.min_expected_report_p95_ms)
            manifest["reports"].append(summary)

        after_metrics = summarize_metrics()
        manifest["metrics_after"] = after_metrics

        jitter_after = float(after_metrics["gateway_udp_jitter_ms_last"])
        loss_delta = float(after_metrics["gateway_udp_loss_estimated_total"]) - float(before_metrics["gateway_udp_loss_estimated_total"])
        reorder_delta = float(after_metrics["gateway_udp_reorder_drop_total"]) - float(before_metrics["gateway_udp_reorder_drop_total"])
        duplicate_delta = float(after_metrics["gateway_udp_duplicate_drop_total"]) - float(before_metrics["gateway_udp_duplicate_drop_total"])

        before_jitter = float(before_metrics["gateway_udp_jitter_ms_last"])
        if jitter_after < args.min_expected_jitter_ms or jitter_after <= before_jitter:
            raise AssertionError(
                f"netem rehearsal did not raise jitter enough: before={before_jitter} "
                f"after={jitter_after} threshold={args.min_expected_jitter_ms}"
            )
        if args.loss_percent > 0 and loss_delta <= 0:
            raise AssertionError(f"netem rehearsal did not move loss metric: delta={loss_delta}")

        manifest["metric_deltas"] = {
            "loss_delta": loss_delta,
            "reorder_delta": reorder_delta,
            "duplicate_delta": duplicate_delta,
            "jitter_after_ms": jitter_after,
        }

        manifest_path = artifact_root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(
            "PASS fps-netem-rehearsal: "
            f"run_id={args.run_id} loss_delta={loss_delta:.0f} "
            f"reorder_delta={reorder_delta:.0f} duplicate_delta={duplicate_delta:.0f} "
            f"jitter_after_ms={jitter_after:.1f} manifest={manifest_path.relative_to(REPO_ROOT)}"
        )
        return 0
    except Exception as exc:  # noqa: BLE001
        manifest["error"] = str(exc)
        failed_path = artifact_root / "manifest.failed.json"
        failed_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            invoke_deploy("down", env_file=env_file, detached=False, build=False, log_path=artifact_root / "down.log")
        except Exception as exc:  # noqa: BLE001
            print(f"[warn] stack cleanup failed: {exc}")


if __name__ == "__main__":
    raise SystemExit(main())
