import unittest
from pathlib import Path
from unittest.mock import patch

import capture_phase5_evidence as phase5


class CapturePhase5EvidenceTests(unittest.TestCase):
    def test_linux_loadgen_build_can_skip_host_visibility_for_container_modes(self):
        with patch.object(phase5, "run_command") as run_command:
            with patch("pathlib.Path.exists", return_value=False):
                result = phase5.ensure_linux_loadgen_built(
                    Path("build/test-loadgen.log"),
                    require_host_visibility=False,
                )

        self.assertEqual(
            phase5.REPO_ROOT / phase5.LINUX_LOADGEN_BUILD_DIR / "stack_loadgen",
            result,
        )
        command = run_command.call_args.args[0]
        self.assertIn("test -x ./build-linux-loadgen/stack_loadgen", command[-1])

    def test_linux_loadgen_build_still_requires_host_visibility_for_host_modes(self):
        with patch.object(phase5, "run_command"):
            with patch("pathlib.Path.exists", return_value=False):
                with self.assertRaisesRegex(
                    RuntimeError,
                    "linux stack_loadgen executable not found after build",
                ):
                    phase5.ensure_linux_loadgen_built(
                        Path("build/test-loadgen.log"),
                        require_host_visibility=True,
                    )


if __name__ == "__main__":
    unittest.main()
