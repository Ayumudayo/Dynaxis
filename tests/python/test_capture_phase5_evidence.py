import unittest
from tempfile import TemporaryDirectory
from pathlib import Path
from unittest.mock import patch

import capture_phase5_evidence as phase5


class CapturePhase5EvidenceTests(unittest.TestCase):
    def test_linux_loadgen_build_discovers_marker_path_for_container_modes(self):
        with TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            marker_path = repo_root / phase5.LINUX_LOADGEN_BUILD_DIR / phase5.LINUX_LOADGEN_PATH_MARKER

            def fake_run(command, *, label, log_path, extra_env=None, cwd=None):
                marker_path.parent.mkdir(parents=True, exist_ok=True)
                marker_path.write_text(
                    "build-linux-loadgen/tools/stack_loadgen",
                    encoding="utf-8",
                )

            with patch.object(phase5, "REPO_ROOT", repo_root):
                with patch.object(phase5, "run_command", side_effect=fake_run) as run_command:
                    result = phase5.ensure_linux_loadgen_built(
                        repo_root / "build" / "test-loadgen.log",
                        require_host_visibility=False,
                    )

        self.assertEqual(repo_root / "build-linux-loadgen" / "tools" / "stack_loadgen", result)
        command = run_command.call_args.args[0]
        self.assertIn("find build-linux-loadgen", command[-1])
        self.assertIn(phase5.LINUX_LOADGEN_PATH_MARKER, command[-1])
        self.assertIn("cat /tmp/loadgen-configure.log", command[-1])
        self.assertIn("cat /tmp/loadgen-build.log", command[-1])

    def test_linux_loadgen_build_requires_host_visibility_for_host_modes(self):
        with TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            marker_path = repo_root / phase5.LINUX_LOADGEN_BUILD_DIR / phase5.LINUX_LOADGEN_PATH_MARKER

            def fake_run(command, *, label, log_path, extra_env=None, cwd=None):
                marker_path.parent.mkdir(parents=True, exist_ok=True)
                marker_path.write_text(
                    "build-linux-loadgen/tools/stack_loadgen",
                    encoding="utf-8",
                )

            with patch.object(phase5, "REPO_ROOT", repo_root):
                with patch.object(phase5, "run_command", side_effect=fake_run):
                    with self.assertRaisesRegex(
                        RuntimeError,
                        "linux stack_loadgen executable not found after build",
                    ):
                        phase5.ensure_linux_loadgen_built(
                            repo_root / "build" / "test-loadgen.log",
                            require_host_visibility=True,
                        )

    def test_linux_loadgen_build_raises_when_marker_is_missing(self):
        with TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            with patch.object(phase5, "REPO_ROOT", repo_root):
                with patch.object(phase5, "run_command"):
                    with self.assertRaisesRegex(
                        RuntimeError,
                        "linux stack_loadgen path marker not found after build",
                    ):
                        phase5.ensure_linux_loadgen_built(
                            repo_root / "build" / "test-loadgen.log",
                            require_host_visibility=False,
                        )

    def test_container_loadgen_capture_uses_discovered_linux_binary_path(self):
        with TemporaryDirectory() as temp_dir:
            repo_root = Path(temp_dir)
            scenario_path = repo_root / "tools" / "loadgen" / "scenarios" / "mixed_direct_udp_fps_soak.json"
            report_path = repo_root / "build" / "loadgen" / "report.json"
            log_path = repo_root / "build" / "logs" / "loadgen.log"
            loadgen_path = repo_root / "build-linux-loadgen" / "tools" / "stack_loadgen"
            scenario_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.parent.mkdir(parents=True, exist_ok=True)
            scenario_path.write_text("{}", encoding="utf-8")

            with patch.object(phase5, "REPO_ROOT", repo_root):
                with patch.object(phase5, "run_command") as run_command:
                    with patch.object(
                        phase5,
                        "summarize_loadgen_report",
                        return_value={
                            "scenario": "mixed_direct_udp_fps_soak",
                            "success_count": 1,
                            "error_count": 0,
                            "throughput_rps": 1.0,
                            "p95_ms": 1.0,
                            "p99_ms": 1.0,
                            "attach_failures": 0,
                            "udp_bind_successes": 1,
                            "rudp_attach_successes": 0,
                            "rudp_attach_fallbacks": 0,
                            "report_path": "build/loadgen/report.json",
                        },
                    ):
                        phase5.run_container_loadgen_capture(
                            loadgen_path=loadgen_path,
                            scenario_path=scenario_path,
                            report_path=report_path,
                            log_path=log_path,
                            host="127.0.0.1",
                            port=36100,
                            udp_port=7000,
                            network_mode="host",
                        )

        command = run_command.call_args.args[0]
        self.assertIn("./build-linux-loadgen/tools/stack_loadgen", command[-1])


if __name__ == "__main__":
    unittest.main()
