from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from stack_topology import ensure_stack_topology_artifacts
from stack_topology import server_ready_ports


REPO_ROOT = Path(__file__).resolve().parents[2]
DEPLOY_SCRIPT = REPO_ROOT / "scripts" / "deploy_docker.ps1"
BUILD_SCRIPT = REPO_ROOT / "scripts" / "build.ps1"
DEFAULT_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "default.json"
ATTACH_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-attach.example"
OFF_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-off.example"
ROLLOUT_FALLBACK_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-fallback.example"

FPS_PHASE2_SCRIPT = Path(__file__).resolve().with_name("verify_fps_rudp_transport_matrix.py")
MMORPG_PHASE3_SCRIPT = Path(__file__).resolve().with_name("verify_mmorpg_runtime_matrix.py")
RECOVERY_PHASE5_SCRIPT = Path(__file__).resolve().with_name("verify_continuity_recovery_matrix.py")

ADMIN_READY_PORT = 39200
GATEWAY_METRICS_PORTS = (36001, 36002)
DIRECT_GATEWAY_PORTS = (36100, 36101)
STACK_NETWORK = "dynaxis-stack_dynaxis-stack"
STACK_HAPROXY_HOST = "haproxy"
STACK_GATEWAY_HOST = "gateway-1"
LINUX_CONTAINER_REPO_ROOT = "/workspace"
LINUX_LOADGEN_BUILD_DIR = "build-linux-loadgen"


def default_run_id() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%SZ")


def default_execution_mode() -> str:
    return "host" if os.name == "nt" else "container"


def run_command(
    command: list[str],
    *,
    label: str,
    log_path: Path,
    extra_env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> str:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        command,
        cwd=cwd or REPO_ROOT,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        check=False,
        env=env,
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


def invoke_deploy(
    action: str,
    *,
    env_file: Path | None,
    detached: bool,
    build: bool,
    log_path: Path,
) -> None:
    command = [
        "pwsh",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(DEPLOY_SCRIPT),
        "-Action",
        action,
        "-TopologyConfig",
        str(DEFAULT_TOPOLOGY),
    ]
    if env_file is not None:
        command.extend(["-EnvFile", str(env_file)])
    if detached:
        command.append("-Detached")
    if build:
        command.append("-Build")
    extra_env = {
        "SESSION_CONTINUITY_ENABLED": "1",
    }
    if build:
        # The hardening workflow prebuilds `dynaxis-base:latest` locally. Keep
        # follow-up compose rebuilds on the classic builder path so Compose can
        # consume that local image instead of trying to pull it from a registry.
        extra_env["DOCKER_BUILDKIT"] = "0"
        extra_env["COMPOSE_DOCKER_CLI_BUILD"] = "0"
    run_command(command, label=f"deploy:{action}", log_path=log_path, extra_env=extra_env)


def wait_http_ready(port: int, timeout_sec: float) -> None:
    url = f"http://127.0.0.1:{port}/readyz"
    deadline = time.monotonic() + timeout_sec
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=3.0) as response:
                body = response.read().decode("utf-8", errors="replace").strip()
                if response.status == 200 and body == "ready":
                    return
                last_error = f"status={response.status} body={body!r}"
        except urllib.error.HTTPError as exc:
            last_error = f"status={exc.code} body={exc.read().decode('utf-8', errors='replace')!r}"
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
        time.sleep(0.5)
    raise TimeoutError(f"readyz timeout on {url}: {last_error}")


def wait_tcp_ready(host: str, port: int, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise TimeoutError(f"tcp timeout on {host}:{port}: {last_error}")


def wait_stack_ready(timeout_sec: float) -> None:
    topology = ensure_stack_topology_artifacts(DEFAULT_TOPOLOGY)
    wait_http_ready(ADMIN_READY_PORT, timeout_sec)
    for port in GATEWAY_METRICS_PORTS:
        wait_http_ready(port, timeout_sec)
    for port in server_ready_ports(topology).values():
        wait_http_ready(port, timeout_sec)
    for port in DIRECT_GATEWAY_PORTS:
        wait_tcp_ready("127.0.0.1", port, timeout_sec)


def ensure_loadgen_built(log_path: Path) -> Path:
    loadgen_path = REPO_ROOT / "build-windows" / "Release" / "stack_loadgen.exe"
    run_command(
        [
            "pwsh",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(BUILD_SCRIPT),
            "-Config",
            "Release",
            "-Target",
            "stack_loadgen",
        ],
        label="build:stack_loadgen",
        log_path=log_path,
    )
    if not loadgen_path.exists():
        raise RuntimeError(f"stack_loadgen executable not found after build: {loadgen_path}")
    return loadgen_path


def ensure_linux_loadgen_built(log_path: Path, *, require_host_visibility: bool = True) -> Path:
    loadgen_path = REPO_ROOT / LINUX_LOADGEN_BUILD_DIR / "stack_loadgen"
    run_command(
        [
            "docker",
            "run",
            "--rm",
            "-v",
            f"{REPO_ROOT}:{LINUX_CONTAINER_REPO_ROOT}",
            "-w",
            LINUX_CONTAINER_REPO_ROOT,
            "dynaxis-base:latest",
            "bash",
            "-lc",
            # Use a dedicated loadgen build tree so Phase 5 evidence capture does
            # not inherit an earlier `build-linux` cache that was configured from
            # a different container mount path.
            f"rm -rf {LINUX_LOADGEN_BUILD_DIR} && "
            f"cmake --preset linux-release -B {LINUX_LOADGEN_BUILD_DIR} "
            "-DBUILD_SERVER_TESTS=OFF -DBUILD_GTEST_TESTS=OFF -DBUILD_CONTRACT_TESTS=OFF >/tmp/loadgen-configure.log && "
            f"cmake --build {LINUX_LOADGEN_BUILD_DIR} --target stack_loadgen >/tmp/loadgen-build.log && "
            f"test -x ./{LINUX_LOADGEN_BUILD_DIR}/stack_loadgen",
        ],
        label="build:stack_loadgen_linux",
        log_path=log_path,
    )
    if require_host_visibility and not loadgen_path.exists():
        raise RuntimeError(f"linux stack_loadgen executable not found after build: {loadgen_path}")
    return loadgen_path


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


def run_assertion_matrix(
    script: Path,
    scenario: str,
    log_path: Path,
) -> dict[str, str]:
    command = [sys.executable, str(script), "--scenario", scenario, "--no-build"]
    run_command(command, label=f"{script.stem}:{scenario}", log_path=log_path)
    return {
        "scenario": scenario,
        "script": str(script.relative_to(REPO_ROOT)),
        "log_path": str(log_path.relative_to(REPO_ROOT)),
        "verdict": "pass",
    }


def run_loadgen_capture(
    loadgen_path: Path,
    scenario_path: Path,
    report_path: Path,
    log_path: Path,
    *,
    host: str,
    port: int,
    udp_port: int | None,
) -> dict[str, object]:
    command = [
        str(loadgen_path),
        "--host",
        host,
        "--port",
        str(port),
        "--scenario",
        str(scenario_path.relative_to(REPO_ROOT)),
        "--report",
        str(report_path.relative_to(REPO_ROOT)),
    ]
    if udp_port is not None:
        command.extend(["--udp-port", str(udp_port)])
    run_command(command, label=f"loadgen:{scenario_path.stem}", log_path=log_path)
    summary = summarize_loadgen_report(report_path)
    print(
        f"{summary['scenario']}: success={summary['success_count']} errors={summary['error_count']} "
        f"throughput_rps={summary['throughput_rps']} p95_ms={summary['p95_ms']}"
    )
    summary["log_path"] = str(log_path.relative_to(REPO_ROOT))
    return summary


def run_container_loadgen_capture(
    scenario_path: Path,
    report_path: Path,
    log_path: Path,
    *,
    host: str,
    port: int,
    udp_port: int | None,
    network_mode: str = "bridge",
) -> dict[str, object]:
    scenario_rel = scenario_path.relative_to(REPO_ROOT).as_posix()
    report_rel = report_path.relative_to(REPO_ROOT).as_posix()
    command = [
        "docker",
        "run",
        "--rm",
    ]
    if network_mode == "host":
        command.extend(["--network", "host"])
    else:
        command.extend(["--network", STACK_NETWORK])
    command.extend(
        [
        "-v",
        f"{REPO_ROOT}:{LINUX_CONTAINER_REPO_ROOT}",
        "-w",
        LINUX_CONTAINER_REPO_ROOT,
        "dynaxis-base:latest",
        "bash",
        "-lc",
        f"./{LINUX_LOADGEN_BUILD_DIR}/stack_loadgen --host {host} --port {port}"
        + (f" --udp-port {udp_port}" if udp_port is not None else "")
        + f" --scenario {scenario_rel} --report {report_rel}",
        ]
    )
    run_command(command, label=f"container-loadgen:{scenario_path.stem}", log_path=log_path)
    summary = summarize_loadgen_report(report_path)
    print(
        f"{summary['scenario']}: success={summary['success_count']} errors={summary['error_count']} "
        f"throughput_rps={summary['throughput_rps']} p95_ms={summary['p95_ms']}"
    )
    summary["log_path"] = str(log_path.relative_to(REPO_ROOT))
    return summary


def run_loadgen_group(
    *,
    loadgen_path: Path,
    env_file: Path | None,
    group_name: str,
    specs: list[dict[str, object]],
    log_root: Path,
    build: bool,
    timeout_sec: float,
) -> list[dict[str, object]]:
    invoke_deploy("down", env_file=env_file, detached=False, build=False, log_path=log_root / f"{group_name}.preclean.log")
    invoke_deploy("up", env_file=env_file, detached=True, build=build, log_path=log_root / f"{group_name}.up.log")
    wait_stack_ready(timeout_sec)
    wait_ports = sorted(
        {
            int(spec["wait_host_port"])
            for spec in specs
            if spec.get("wait_host_port") is not None
        }
    )
    for wait_port in wait_ports:
        wait_tcp_ready("127.0.0.1", wait_port, timeout_sec)

    reports: list[dict[str, object]] = []
    for spec in specs:
        if spec.get("containerized"):
            reports.append(
                run_container_loadgen_capture(
                    spec["scenario_path"],
                    spec["report_path"],
                    log_root / spec["log_name"],
                    host=str(spec["host"]),
                    port=int(spec["port"]),
                    udp_port=int(spec["udp_port"]) if spec.get("udp_port") is not None else None,
                    network_mode=str(spec.get("container_network_mode", "bridge")),
                )
            )
        else:
            reports.append(
                run_loadgen_capture(
                    loadgen_path,
                    spec["scenario_path"],
                    spec["report_path"],
                    log_root / spec["log_name"],
                    host=str(spec.get("host", "127.0.0.1")),
                    port=int(spec["port"]),
                    udp_port=int(spec["udp_port"]) if spec.get("udp_port") is not None else None,
                )
            )

    invoke_deploy("down", env_file=env_file, detached=False, build=False, log_path=log_root / f"{group_name}.down.log")
    return reports


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture the first Phase 5 quantitative evidence set.")
    parser.add_argument("--run-id", default=default_run_id())
    parser.add_argument("--stack-ready-timeout-sec", type=float, default=90.0)
    parser.add_argument(
        "--execution-mode",
        choices=("auto", "host", "container", "hostnet-container"),
        default="auto",
        help="Choose whether loadgen runs use host binaries or same-network Linux containers.",
    )
    parser.add_argument(
        "--capture-set",
        choices=("fps-direct-only", "budget-hardening", "rudp-success-only", "rudp-off-only"),
        default="fps-direct-only",
        help="Select which loadgen evidence set to capture.",
    )
    parser.add_argument("--include-assertion-matrices", action="store_true")
    parser.add_argument("--include-budget-hardening", action="store_true")
    parser.add_argument("--skip-loadgen", action="store_true")
    args = parser.parse_args()

    capture_set = args.capture_set
    execution_mode = args.execution_mode
    if execution_mode == "auto":
        execution_mode = default_execution_mode()
    if args.include_budget_hardening:
        if capture_set != "fps-direct-only":
            parser.error("--include-budget-hardening cannot be combined with --capture-set")
        capture_set = "budget-hardening"

    artifact_root = REPO_ROOT / "build" / "phase5-evidence" / args.run_id
    fps_root = artifact_root / "fps"
    mmorpg_root = artifact_root / "mmorpg"
    continuity_root = artifact_root / "continuity"
    loadgen_root = artifact_root / "loadgen"
    artifact_root.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "run_id": args.run_id,
        "artifact_root": str(artifact_root.relative_to(REPO_ROOT)),
        "capture_set": capture_set,
        "execution_mode": execution_mode,
        "assertion_matrices": [],
        "loadgen_reports": [],
    }

    try:
        if args.include_assertion_matrices:
            manifest["assertion_matrices"] = [
                run_assertion_matrix(
                    FPS_PHASE2_SCRIPT,
                    "phase2-acceptance",
                    fps_root / "phase2-acceptance.log",
                ),
                run_assertion_matrix(
                    MMORPG_PHASE3_SCRIPT,
                    "phase3-acceptance",
                    mmorpg_root / "phase3-acceptance.log",
                ),
                run_assertion_matrix(
                    RECOVERY_PHASE5_SCRIPT,
                    "phase5-recovery-baseline",
                    continuity_root / "phase5-recovery-baseline.log",
                ),
            ]

        if not args.skip_loadgen:
            if execution_mode in ("container", "hostnet-container"):
                loadgen_path = ensure_linux_loadgen_built(
                    loadgen_root / "build-stack-loadgen-linux.log",
                    require_host_visibility=False,
                )
            else:
                loadgen_path = ensure_loadgen_built(loadgen_root / "build-stack-loadgen.log")
            loadgen_report_root = REPO_ROOT / "build" / "loadgen"
            loadgen_report_root.mkdir(parents=True, exist_ok=True)
            if execution_mode == "container":
                direct_host = STACK_GATEWAY_HOST
                direct_port = 6000
                direct_containerized = True
                direct_container_network = "bridge"
                direct_wait_host_port: int | None = None
            elif execution_mode == "hostnet-container":
                direct_host = "127.0.0.1"
                direct_port = 36100
                direct_containerized = True
                direct_container_network = "host"
                direct_wait_host_port = 36100
            else:
                direct_host = "127.0.0.1"
                direct_port = 36100
                direct_containerized = False
                direct_container_network = "bridge"
                direct_wait_host_port = 36100
            direct_udp_port = 7000

            attach_specs = [
                {
                    "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_udp_fps_soak.json",
                    "report_path": loadgen_report_root / f"mixed_direct_udp_fps_soak.{args.run_id}.host.json",
                    "log_name": "mixed_direct_udp_fps_soak.log",
                    "host": direct_host,
                    "port": direct_port,
                    "udp_port": direct_udp_port,
                    "containerized": direct_containerized,
                    "container_network_mode": direct_container_network,
                    "wait_host_port": direct_wait_host_port,
                },
                {
                    "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_fps_soak.json",
                    "report_path": loadgen_report_root / f"mixed_direct_rudp_fps_soak.{args.run_id}.host.json",
                    "log_name": "mixed_direct_rudp_fps_soak.log",
                    "host": direct_host,
                    "port": direct_port,
                    "udp_port": direct_udp_port,
                    "containerized": direct_containerized,
                    "container_network_mode": direct_container_network,
                    "wait_host_port": direct_wait_host_port,
                },
            ]
            rudp_success_only_specs = [
                {
                    "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_soak_long.json",
                    "report_path": loadgen_report_root / f"mixed_direct_rudp_soak_long.{args.run_id}.host.json",
                    "log_name": "mixed_direct_rudp_soak_long.log",
                    "host": direct_host,
                    "port": direct_port,
                    "udp_port": direct_udp_port,
                    "containerized": direct_containerized,
                    "container_network_mode": direct_container_network,
                    "wait_host_port": direct_wait_host_port,
                }
            ]
            rudp_off_only_specs = [
                {
                    "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_soak_long.json",
                    "report_path": loadgen_report_root / f"mixed_direct_rudp_soak_long.{args.run_id}.off.host.json",
                    "log_name": "mixed_direct_rudp_soak_long.off.log",
                    "host": direct_host,
                    "port": direct_port,
                    "udp_port": direct_udp_port,
                    "containerized": direct_containerized,
                    "container_network_mode": direct_container_network,
                    "wait_host_port": direct_wait_host_port,
                }
            ]

            if capture_set == "budget-hardening":
                baseline_specs = [
                    {
                        "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_session_soak_long.json",
                        "report_path": loadgen_report_root / f"mixed_session_soak_long.{args.run_id}.json",
                        "log_name": "mixed_session_soak_long.log",
                        "host": STACK_HAPROXY_HOST,
                        "port": 6000,
                        "udp_port": None,
                        "containerized": True,
                        "wait_host_port": 6000,
                        "container_network_mode": "bridge",
                    }
                ]
                if execution_mode == "hostnet-container":
                    baseline_specs = [
                        {
                            "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_session_soak_long.json",
                            "report_path": loadgen_report_root / f"mixed_session_soak_long.{args.run_id}.json",
                            "log_name": "mixed_session_soak_long.log",
                            "host": "127.0.0.1",
                            "port": 6000,
                            "udp_port": None,
                            "containerized": True,
                            "wait_host_port": 6000,
                            "container_network_mode": "host",
                        }
                    ]
                loadgen_reports = run_loadgen_group(
                    loadgen_path=loadgen_path,
                    env_file=None,
                    group_name="baseline-tcp-env",
                    specs=baseline_specs,
                    log_root=loadgen_root,
                    build=True,
                    timeout_sec=args.stack_ready_timeout_sec,
                )

                attach_specs = [
                    {
                        "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_udp_soak_long.json",
                        "report_path": loadgen_report_root / f"mixed_direct_udp_soak_long.{args.run_id}.host.json",
                        "log_name": "mixed_direct_udp_soak_long.log",
                        "host": direct_host,
                        "port": direct_port,
                        "udp_port": direct_udp_port,
                        "containerized": direct_containerized,
                        "container_network_mode": direct_container_network,
                        "wait_host_port": direct_wait_host_port,
                    },
                    {
                        "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_soak_long.json",
                        "report_path": loadgen_report_root / f"mixed_direct_rudp_soak_long.{args.run_id}.host.json",
                        "log_name": "mixed_direct_rudp_soak_long.log",
                        "host": direct_host,
                        "port": direct_port,
                        "udp_port": direct_udp_port,
                        "containerized": direct_containerized,
                        "container_network_mode": direct_container_network,
                        "wait_host_port": direct_wait_host_port,
                    },
                    *attach_specs,
                ]
            elif capture_set == "rudp-success-only":
                loadgen_reports = []
                attach_specs = rudp_success_only_specs
            elif capture_set == "rudp-off-only":
                loadgen_reports = []
                attach_specs = []
            else:
                loadgen_reports = []

            if attach_specs:
                loadgen_reports.extend(
                    run_loadgen_group(
                        loadgen_path=loadgen_path,
                        env_file=ATTACH_ENV_FILE,
                        group_name="attach-env",
                        specs=attach_specs,
                        log_root=loadgen_root,
                        build=capture_set != "budget-hardening",
                        timeout_sec=args.stack_ready_timeout_sec,
                    )
                )

            if capture_set == "budget-hardening":
                loadgen_reports.extend(
                    run_loadgen_group(
                        loadgen_path=loadgen_path,
                        env_file=ROLLOUT_FALLBACK_ENV_FILE,
                        group_name="fallback-env",
                        specs=[
                            {
                                "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_soak_long.json",
                                "report_path": loadgen_report_root / f"mixed_direct_rudp_soak_long.{args.run_id}.fallback.host.json",
                                "log_name": "mixed_direct_rudp_soak_long.fallback.log",
                                "host": direct_host,
                                "port": direct_port,
                                "udp_port": direct_udp_port,
                                "containerized": direct_containerized,
                                "container_network_mode": direct_container_network,
                                "wait_host_port": direct_wait_host_port,
                            }
                        ],
                        log_root=loadgen_root,
                        build=False,
                        timeout_sec=args.stack_ready_timeout_sec,
                    )
                )
                loadgen_reports.extend(
                    run_loadgen_group(
                        loadgen_path=loadgen_path,
                        env_file=OFF_ENV_FILE,
                        group_name="off-env",
                        specs=[
                            {
                                "scenario_path": REPO_ROOT / "tools" / "loadgen" / "scenarios" / "mixed_direct_rudp_soak_long.json",
                                "report_path": loadgen_report_root / f"mixed_direct_rudp_soak_long.{args.run_id}.off.host.json",
                                "log_name": "mixed_direct_rudp_soak_long.off.log",
                                "host": direct_host,
                                "port": direct_port,
                                "udp_port": direct_udp_port,
                                "containerized": direct_containerized,
                                "container_network_mode": direct_container_network,
                                "wait_host_port": direct_wait_host_port,
                            }
                        ],
                        log_root=loadgen_root,
                        build=False,
                        timeout_sec=args.stack_ready_timeout_sec,
                    )
                )
            elif capture_set == "rudp-off-only":
                loadgen_reports.extend(
                    run_loadgen_group(
                        loadgen_path=loadgen_path,
                        env_file=OFF_ENV_FILE,
                        group_name="off-env",
                        specs=rudp_off_only_specs,
                        log_root=loadgen_root,
                        build=True,
                        timeout_sec=args.stack_ready_timeout_sec,
                    )
                )

            manifest["loadgen_reports"] = loadgen_reports

        manifest_path = artifact_root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"PASS phase5-capture: manifest={manifest_path.relative_to(REPO_ROOT)}")
        return 0
    except Exception as exc:  # noqa: BLE001
        manifest_path = artifact_root / "manifest.failed.json"
        manifest["error"] = str(exc)
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"FAIL: {exc}")
        return 1
    finally:
        if not args.skip_loadgen:
            try:
                invoke_deploy("down", env_file=ATTACH_ENV_FILE, detached=False, build=False, log_path=loadgen_root / "deploy-final-cleanup.log")
            except Exception as exc:  # noqa: BLE001
                print(f"[warn] stack cleanup failed: {exc}")


if __name__ == "__main__":
    raise SystemExit(main())
