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
from verify_worlds_kubernetes_kind_continuity import GATEWAY_SERVICE_PORT
from verify_worlds_kubernetes_kind_continuity import wait_for_tcp_port_forward
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


def write_impairment_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "gateway-ingress-impairment.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"gateway impairment report: {report_path}")


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


def fresh_login(host: str, port: int, user: str) -> tuple[ChatClient, dict]:
    client = ChatClient(host=host, port=port)
    client.connect()
    login = client.login(user, "", timeout_sec=8.0)
    return client, login


def wait_for_fresh_login_success(host: str, port: int, user: str, timeout_seconds: float) -> tuple[ChatClient, dict]:
    deadline = time.time() + timeout_seconds
    last_error = "fresh login did not succeed"
    while time.time() < deadline:
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            login = client.login(user, "", timeout_sec=5.0)
            return client, login
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
            client.close()
            time.sleep(0.5)
    raise RuntimeError(f"fresh login did not recover: {last_error}")


def wait_for_fresh_ingress_rejection(
    host: str,
    port: int,
    metrics_url: str,
    timeout_seconds: float,
) -> dict[str, float]:
    reject_before = read_metric(metrics_url, "gateway_ingress_reject_not_ready_total")
    deadline = time.time() + timeout_seconds
    last_error = "no rejection observed"
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            client.login(f"impairment_login_{int(time.time())}_{attempt}", "", timeout_sec=2.0)
            last_error = "fresh login unexpectedly succeeded while gateway was not ready"
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

    raise RuntimeError(f"gateway ingress rejection was not observed: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a live kind client-visible gateway ingress impairment proof under Redis loss."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    unique_suffix = f"{int(time.time() * 1000)}-{os.getpid()}"
    cluster_name = f"dynaxis-gateway-impair-{unique_suffix}"
    namespace = f"dynaxis-gateway-impair-{unique_suffix}"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-gateway-impair-") as temp_dir_raw:
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

        existing: ChatClient | None = None
        recovered: ChatClient | None = None
        try:
            run(up_command)
            with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                    with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_METRICS_PORT) as gateway1_metrics_forward:
                        with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT) as gateway2_metrics_forward:
                            gateway1_host, gateway1_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                            gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                            gateway1_metrics_url = gateway1_metrics_forward.base_url
                            gateway2_metrics_url = gateway2_metrics_forward.base_url

                            wait_for_gateway_ready(gateway1_metrics_url, expected_ready=True, timeout_seconds=30.0)
                            wait_for_gateway_ready(gateway2_metrics_url, expected_ready=True, timeout_seconds=30.0)

                            room = f"kind_gateway_impair_room_{int(time.time())}"
                            message_outage = f"kind_gateway_impair_msg_{int(time.time() * 1000)}"
                            message_recovered = f"kind_gateway_impair_recovered_{int(time.time() * 1000)}"
                            existing_user = f"kind_gateway_impair_existing_{int(time.time())}"
                            recovered_user = f"kind_gateway_impair_recovered_{int(time.time())}"

                            existing = ChatClient(host=gateway1_host, port=gateway1_port)
                            existing.connect()
                            login = existing.login(existing_user, "")
                            if login.get("effective_user") != existing_user:
                                raise RuntimeError(f"unexpected initial login payload: {login}")
                            existing.join_room(room, existing_user)

                            before_gateway1_dep = read_metric(
                                gateway1_metrics_url,
                                "runtime_dependency_ready",
                                {"name": "redis", "required": "true"},
                            )
                            before_gateway2_dep = read_metric(
                                gateway2_metrics_url,
                                "runtime_dependency_ready",
                                {"name": "redis", "required": "true"},
                            )
                            if before_gateway1_dep != 1.0 or before_gateway2_dep != 1.0:
                                raise RuntimeError(
                                    f"gateway redis dependency was not healthy before outage: "
                                    f"gw1={before_gateway1_dep} gw2={before_gateway2_dep}"
                                )

                            scale_redis(cluster_name, namespace, 0)
                            wait_for_redis_pod_absent(cluster_name, namespace, timeout_seconds=60.0)

                            outage_gateway1_dep = wait_for_metric_value(
                                gateway1_metrics_url,
                                "runtime_dependency_ready",
                                lambda value: value == 0.0,
                                labels={"name": "redis", "required": "true"},
                                timeout_seconds=30.0,
                            )
                            outage_gateway2_dep = wait_for_metric_value(
                                gateway2_metrics_url,
                                "runtime_dependency_ready",
                                lambda value: value == 0.0,
                                labels={"name": "redis", "required": "true"},
                                timeout_seconds=30.0,
                            )
                            outage_gateway1_deps_ok = read_metric(gateway1_metrics_url, "runtime_dependencies_ok")
                            outage_gateway2_deps_ok = read_metric(gateway2_metrics_url, "runtime_dependencies_ok")
                            gateway1_ready = wait_for_gateway_ready(
                                gateway1_metrics_url,
                                expected_ready=False,
                                timeout_seconds=30.0,
                            )
                            gateway2_ready = wait_for_gateway_ready(
                                gateway2_metrics_url,
                                expected_ready=False,
                                timeout_seconds=30.0,
                            )

                            existing.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message_outage))
                            existing.wait_for_self_chat(room, message_outage, 20.0)

                            reject_stats = wait_for_fresh_ingress_rejection(
                                gateway2_host,
                                gateway2_port,
                                gateway2_metrics_url,
                                timeout_seconds=15.0,
                            )

                            scale_redis(cluster_name, namespace, 1)
                            wait_for_redis_pod_ready(cluster_name, namespace, timeout_seconds=args.wait_timeout_seconds)

                            recovered_gateway1_dep = wait_for_metric_value(
                                gateway1_metrics_url,
                                "runtime_dependency_ready",
                                lambda value: value == 1.0,
                                labels={"name": "redis", "required": "true"},
                                timeout_seconds=60.0,
                            )
                            recovered_gateway2_dep = wait_for_metric_value(
                                gateway2_metrics_url,
                                "runtime_dependency_ready",
                                lambda value: value == 1.0,
                                labels={"name": "redis", "required": "true"},
                                timeout_seconds=60.0,
                            )
                            recovered_gateway1_deps_ok = read_metric(gateway1_metrics_url, "runtime_dependencies_ok")
                            recovered_gateway2_deps_ok = read_metric(gateway2_metrics_url, "runtime_dependencies_ok")
                            wait_for_gateway_ready(gateway1_metrics_url, expected_ready=True, timeout_seconds=30.0)
                            wait_for_gateway_ready(gateway2_metrics_url, expected_ready=True, timeout_seconds=30.0)

                            recovered, recovered_login = wait_for_fresh_login_success(
                                gateway2_host,
                                gateway2_port,
                                recovered_user,
                                timeout_seconds=60.0,
                            )
                            if recovered_login.get("effective_user") != recovered_user:
                                raise RuntimeError(f"unexpected recovered login payload: {recovered_login}")
                            recovered.join_room(room, recovered_user)
                            recovered.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message_recovered))
                            recovered.wait_for_self_chat(room, message_recovered, 20.0)

                            report = {
                                "proof": "worlds-kubernetes-kind-gateway-ingress-impairment",
                                "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                                "existing_session": {
                                    "gateway": "gateway-1",
                                    "user": existing_user,
                                    "outage_room_message": message_outage,
                                },
                                "fresh_ingress_after_recovery": {
                                    "gateway": "gateway-2",
                                    "user": recovered_user,
                                    "recovered_room_message": message_recovered,
                                },
                                "outage": {
                                    "gateway1_readyz": {"status": gateway1_ready[0], "body": gateway1_ready[1]},
                                    "gateway2_readyz": {"status": gateway2_ready[0], "body": gateway2_ready[1]},
                                    "gateway1_dependency_redis": outage_gateway1_dep,
                                    "gateway2_dependency_redis": outage_gateway2_dep,
                                    "gateway1_dependencies_ok": outage_gateway1_deps_ok,
                                    "gateway2_dependencies_ok": outage_gateway2_deps_ok,
                                    "gateway2_ingress_reject_not_ready": reject_stats,
                                },
                                "recovered": {
                                    "gateway1_dependency_redis": recovered_gateway1_dep,
                                    "gateway2_dependency_redis": recovered_gateway2_dep,
                                    "gateway1_dependencies_ok": recovered_gateway1_deps_ok,
                                    "gateway2_dependencies_ok": recovered_gateway2_deps_ok,
                                },
                            }
                            write_impairment_report(report, args.artifact_dir)
                            print(
                                "PASS worlds-kubernetes-kind-gateway-ingress-impairment: "
                                f"reject_delta={reject_stats['delta']:.0f} recovered_user={recovered_user}"
                            )
                            return 0
        except Exception as exc:
            print(f"FAIL worlds-kubernetes-kind-gateway-ingress-impairment: {exc}")
            return 1
        finally:
            if existing is not None:
                existing.close()
            if recovered is not None:
                recovered.close()
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


if __name__ == "__main__":
    raise SystemExit(main())
