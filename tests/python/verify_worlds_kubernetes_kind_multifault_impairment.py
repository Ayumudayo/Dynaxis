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
    wait_for_tcp_port_forward,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_metrics_budget import GATEWAY_METRICS_PORT
from verify_worlds_kubernetes_kind_redis_outage import (
    read_metric,
    scale_redis,
    wait_for_metric_value,
    wait_for_redis_pod_absent,
    wait_for_redis_pod_ready,
)
from verify_worlds_kubernetes_kind_restart import get_pod_uids, wait_for_pod_uid_change
from verify_worlds_kubernetes_kind_resume_impairment import (
    wait_for_fresh_login_success,
    wait_for_gateway_ready,
    wait_for_resume_rejection,
    wait_for_resume_success,
)


REPO_ROOT = Path(__file__).resolve().parents[2]


def write_multifault_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "multifault-impairment.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"multi-fault impairment report: {report_path}")


def delete_selected_pods(cluster_name: str, namespace: str, selector: str) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "delete",
            "pods",
            "-l",
            selector,
        ]
    )


def wait_for_deployment_available(
    cluster_name: str,
    namespace: str,
    deployment_name: str,
    timeout_seconds: int,
) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "wait",
            "--for=condition=Available",
            f"deployment/{deployment_name}",
            f"--timeout={timeout_seconds}s",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a live kind multi-fault impairment proof for Redis outage plus gateway pod churn."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    unique_suffix = f"{int(time.time() * 1000)}-{os.getpid()}"
    cluster_name = f"dynaxis-multifault-{unique_suffix}"
    namespace = f"dynaxis-multifault-{unique_suffix}"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-multifault-") as temp_dir_raw:
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

            user = f"kind_multifault_{int(time.time())}"
            initial_room = f"kind_multifault_room_{int(time.time())}"
            recovery_room = f"kind_multifault_recovery_room_{int(time.time())}"
            recovered_message = f"kind_multifault_recovered_{int(time.time() * 1000)}"
            logical_session_id = ""
            resume_token = ""

            outage_dependency_before_restart = 0.0
            outage_ready_before_restart: tuple[int, str] = (0, "")
            reject_before_restart: dict[str, float]

            with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT) as gateway2_metrics_forward:
                        gateway1_host, gateway1_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        gateway2_metrics_url = gateway2_metrics_forward.base_url
                        wait_for_gateway_ready(gateway2_metrics_url, expected_ready=True, timeout_seconds=30.0)

                        first = ChatClient(host=gateway1_host, port=gateway1_port)
                        first.connect()
                        login = first.login(user, "")
                        logical_session_id, resume_token = assert_initial_login(login, user)
                        first.join_room(initial_room, user)
                        first.close()
                        first = None

                        scale_redis(cluster_name, namespace, 0)
                        wait_for_redis_pod_absent(cluster_name, namespace, timeout_seconds=60.0)
                        outage_dependency_before_restart = wait_for_metric_value(
                            gateway2_metrics_url,
                            "runtime_dependency_ready",
                            lambda value: value == 0.0,
                            labels={"name": "redis", "required": "true"},
                            timeout_seconds=30.0,
                        )
                        outage_ready_before_restart = wait_for_gateway_ready(
                            gateway2_metrics_url,
                            expected_ready=False,
                            timeout_seconds=30.0,
                        )
                        reject_before_restart = wait_for_resume_rejection(
                            gateway2_host,
                            gateway2_port,
                            resume_token,
                            gateway2_metrics_url,
                            timeout_seconds=15.0,
                        )

            gateway_selector = "app.kubernetes.io/name=gateway-2"
            gateway_uids_before = get_pod_uids(cluster_name, namespace, gateway_selector)
            if not gateway_uids_before:
                raise RuntimeError("gateway-2 pods were missing before churn")

            delete_selected_pods(cluster_name, namespace, gateway_selector)
            gateway_uids_after = wait_for_pod_uid_change(
                cluster_name,
                namespace,
                gateway_selector,
                gateway_uids_before,
                60.0,
            )
            wait_for_redis_pod_absent(cluster_name, namespace, timeout_seconds=5.0)

            scale_redis(cluster_name, namespace, 1)
            wait_for_redis_pod_ready(cluster_name, namespace, timeout_seconds=args.wait_timeout_seconds)
            wait_for_deployment_available(
                cluster_name,
                namespace,
                "gateway-2",
                args.wait_timeout_seconds,
            )
            restarted_gateway2_pod = next(iter(gateway_uids_after))

            with PortForward(
                cluster_name,
                namespace,
                restarted_gateway2_pod,
                GATEWAY_SERVICE_PORT,
                resource_kind="pod",
            ) as gateway2_forward:
                with PortForward(
                    cluster_name,
                    namespace,
                    restarted_gateway2_pod,
                    GATEWAY_METRICS_PORT,
                    resource_kind="pod",
                ) as gateway2_metrics_forward:
                    gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=60.0)
                    gateway2_metrics_url = gateway2_metrics_forward.base_url

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
                        "proof": "worlds-kubernetes-kind-multifault-impairment",
                        "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                        "session": {
                            "user": user,
                            "logical_session_id": logical_session_id,
                            "initial_room": initial_room,
                            "recovery_room": recovery_room,
                        },
                        "outage_before_gateway_churn": {
                            "gateway2_readyz": {
                                "status": outage_ready_before_restart[0],
                                "body": outage_ready_before_restart[1],
                            },
                            "gateway2_dependency_redis": outage_dependency_before_restart,
                            "gateway2_resume_reject_not_ready": reject_before_restart,
                        },
                        "gateway_churn_during_outage": {
                            "gateway2_pods_before": gateway_uids_before,
                            "gateway2_pods_after": gateway_uids_after,
                            "restarted_gateway2_pod": restarted_gateway2_pod,
                            "redis_still_absent": True,
                        },
                        "recovered": {
                            "gateway2_dependency_redis": recovered_dependency,
                            "gateway2_dependencies_ok": recovered_deps_ok,
                            "resumed_world_id": resumed_login.get("world_id"),
                            "resumed": resumed_login.get("resumed"),
                        },
                    }
                    write_multifault_report(report, args.artifact_dir)
                    print(
                        "PASS worlds-kubernetes-kind-multifault-impairment: "
                        f"reject_before={reject_before_restart['delta']:.0f} restarted_pods={len(gateway_uids_after)}"
                    )
                    return 0
        except Exception as exc:
            print(f"FAIL worlds-kubernetes-kind-multifault-impairment: {exc}")
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
