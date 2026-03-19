from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path

from session_continuity_common import ChatClient
from session_continuity_common import lp_utf8
from session_continuity_common import MSG_CHAT_SEND
from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_continuity import (
    GATEWAY_SERVICE_PORT,
    assert_initial_login,
    assert_resumed_login,
    wait_for_tcp_port_forward,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_metrics_budget import GATEWAY_METRICS_PORT
from verify_worlds_kubernetes_kind_redis_outage import (
    http_text,
    read_metric,
    scale_redis,
    wait_for_metric_value,
    wait_for_redis_pod_absent,
    wait_for_redis_pod_ready,
)


REPO_ROOT = Path(__file__).resolve().parents[2]


def write_resume_impairment_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "resume-impairment.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"resume impairment report: {report_path}")


def wait_for_gateway_ready(base_url: str, expected_ready: bool, timeout_seconds: float) -> tuple[int, str]:
    deadline = time.time() + timeout_seconds
    last_status = 0
    last_body = ""
    while time.time() < deadline:
        last_status, last_body = http_text(base_url, "/readyz")
        if expected_ready:
            if last_status == 200 and last_body == "ready":
                return last_status, last_body
        else:
            if last_status != 200 or last_body != "ready":
                return last_status, last_body
        time.sleep(0.5)
    raise RuntimeError(
        f"timeout waiting for gateway ready={expected_ready}: status={last_status} body={last_body!r}"
    )


def wait_for_resume_rejection(
    host: str,
    port: int,
    resume_token: str,
    metrics_url: str,
    timeout_seconds: float,
) -> dict[str, float]:
    reject_before = read_metric(metrics_url, "gateway_ingress_reject_not_ready_total")
    deadline = time.time() + timeout_seconds
    last_error = "no rejection observed"
    while time.time() < deadline:
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            client.login("ignored_resume_user", "resume:" + resume_token, timeout_sec=2.0)
            last_error = "resume unexpectedly succeeded while gateway was not ready"
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
        finally:
            client.close()

        reject_after = read_metric(metrics_url, "gateway_ingress_reject_not_ready_total")
        if reject_after > reject_before:
            return {
                "before": reject_before,
                "after": reject_after,
                "delta": reject_after - reject_before,
            }
        time.sleep(0.5)
    raise RuntimeError(f"resume rejection was not observed: {last_error}")


def wait_for_resume_success(
    host: str,
    port: int,
    resume_token: str,
    user: str,
    logical_session_id: str,
    timeout_seconds: float,
) -> tuple[ChatClient, dict]:
    deadline = time.time() + timeout_seconds
    last_error = "resume did not recover"
    while time.time() < deadline:
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            login = client.login("ignored_resume_user", "resume:" + resume_token, timeout_sec=5.0)
            assert_resumed_login(login, user, logical_session_id)
            return client, login
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
            client.close()
            time.sleep(0.5)
    raise RuntimeError(f"resume did not recover: {last_error}")


def wait_for_fresh_login_success(host: str, port: int, timeout_seconds: float) -> None:
    deadline = time.time() + timeout_seconds
    last_error = "fresh login did not recover"
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            login = client.login(f"resume_probe_{int(time.time())}_{attempt}", "", timeout_sec=5.0)
            if login.get("effective_user"):
                client.close()
                return
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
        finally:
            client.close()
        time.sleep(0.5)
    raise RuntimeError(f"fresh login did not recover before resume: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a live kind continuity-resume availability impairment proof under required Redis outage."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    unique_suffix = f"{int(time.time() * 1000)}-{os.getpid()}"
    cluster_name = f"dynaxis-resume-impair-{unique_suffix}"
    namespace = f"dynaxis-resume-impair-{unique_suffix}"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-resume-impair-") as temp_dir_raw:
        output_dir = Path(temp_dir_raw)
        up_command = [
            sys.executable,
            str(RUNNER),
            "up",
            "--cluster-name",
            cluster_name,
            "--topology-config",
            str(DEFAULT_TOPOLOGY),
            "--namespace",
            namespace,
            "--output-dir",
            str(output_dir),
            "--wait-timeout-seconds",
            str(args.wait_timeout_seconds),
            "--clean-namespace",
            "--recreate-cluster",
        ]
        down_command = [
            sys.executable,
            str(RUNNER),
            "down",
            "--cluster-name",
            cluster_name,
        ]

        first: ChatClient | None = None
        resumed: ChatClient | None = None
        try:
            run(up_command)
            with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT) as gateway2_metrics_forward:
                        gateway1_host, gateway1_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        gateway2_metrics_url = gateway2_metrics_forward.base_url
                        wait_for_gateway_ready(gateway2_metrics_url, expected_ready=True, timeout_seconds=30.0)

                        user = f"kind_resume_impair_{int(time.time())}"
                        room = f"kind_resume_impair_room_{int(time.time())}"
                        recovery_room = f"kind_resume_impair_recovery_room_{int(time.time())}"
                        recovered_message = f"kind_resume_impair_recovered_{int(time.time() * 1000)}"

                        first = ChatClient(host=gateway1_host, port=gateway1_port)
                        first.connect()
                        login = first.login(user, "")
                        logical_session_id, resume_token = assert_initial_login(login, user)
                        first.join_room(room, user)
                        first.close()
                        first = None

                        before_dependency = read_metric(
                            gateway2_metrics_url,
                            "runtime_dependency_ready",
                            {"name": "redis", "required": "true"},
                        )
                        before_deps_ok = read_metric(gateway2_metrics_url, "runtime_dependencies_ok")
                        if before_dependency != 1.0 or before_deps_ok != 1.0:
                            raise RuntimeError(
                                f"gateway dependency was not healthy before outage: dep={before_dependency} deps_ok={before_deps_ok}"
                            )

                        scale_redis(cluster_name, namespace, 0)
                        wait_for_redis_pod_absent(cluster_name, namespace, timeout_seconds=60.0)
                        outage_dependency = wait_for_metric_value(
                            gateway2_metrics_url,
                            "runtime_dependency_ready",
                            lambda value: value == 0.0,
                            labels={"name": "redis", "required": "true"},
                            timeout_seconds=30.0,
                        )
                        outage_deps_ok = read_metric(gateway2_metrics_url, "runtime_dependencies_ok")
                        outage_ready = wait_for_gateway_ready(gateway2_metrics_url, expected_ready=False, timeout_seconds=30.0)
                        reject_stats = wait_for_resume_rejection(
                            gateway2_host,
                            gateway2_port,
                            resume_token,
                            gateway2_metrics_url,
                            timeout_seconds=15.0,
                        )

                        scale_redis(cluster_name, namespace, 1)
                        wait_for_redis_pod_ready(cluster_name, namespace, timeout_seconds=args.wait_timeout_seconds)
                        recovered_dependency = wait_for_metric_value(
                            gateway2_metrics_url,
                            "runtime_dependency_ready",
                            lambda value: value == 1.0,
                            labels={"name": "redis", "required": "true"},
                            timeout_seconds=60.0,
                        )
                        recovered_deps_ok = read_metric(gateway2_metrics_url, "runtime_dependencies_ok")
                        wait_for_gateway_ready(gateway2_metrics_url, expected_ready=True, timeout_seconds=30.0)
                        wait_for_fresh_login_success(gateway2_host, gateway2_port, timeout_seconds=90.0)

                        resumed, resumed_login = wait_for_resume_success(
                            gateway2_host,
                            gateway2_port,
                            resume_token,
                            user,
                            logical_session_id,
                            timeout_seconds=120.0,
                        )
                        resumed.join_room(recovery_room, user)
                        resumed.send_frame(MSG_CHAT_SEND, lp_utf8(recovery_room) + lp_utf8(recovered_message))
                        resumed.wait_for_self_chat(recovery_room, recovered_message, 20.0)

                        report = {
                            "proof": "worlds-kubernetes-kind-resume-impairment",
                            "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                            "session": {
                                "user": user,
                                "logical_session_id": logical_session_id,
                                "initial_room": room,
                                "recovery_room": recovery_room,
                            },
                            "outage": {
                                "gateway2_readyz": {"status": outage_ready[0], "body": outage_ready[1]},
                                "gateway2_dependency_redis": outage_dependency,
                                "gateway2_dependencies_ok": outage_deps_ok,
                                "gateway2_resume_reject_not_ready": reject_stats,
                            },
                            "recovered": {
                                "gateway2_dependency_redis": recovered_dependency,
                                "gateway2_dependencies_ok": recovered_deps_ok,
                                "resumed_world_id": resumed_login.get("world_id"),
                                "resumed": resumed_login.get("resumed"),
                                "recovery_room": recovery_room,
                            },
                        }
                        write_resume_impairment_report(report, args.artifact_dir)
                        print(
                            "PASS worlds-kubernetes-kind-resume-impairment: "
                            f"reject_delta={reject_stats['delta']:.0f} resumed_world={resumed_login.get('world_id')}"
                        )
                        return 0
        except Exception as exc:
            print(f"FAIL worlds-kubernetes-kind-resume-impairment: {exc}")
            return 1
        finally:
            if first is not None:
                first.close()
            if resumed is not None:
                resumed.close()
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


if __name__ == "__main__":
    raise SystemExit(main())
