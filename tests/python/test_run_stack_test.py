from pathlib import Path
import unittest
from unittest import mock

import run_stack_test


class RunStackTestPrerequisiteTests(unittest.TestCase):
    def test_plugin_script_matrix_requires_runtime_on_stack(self):
        expected_reason = "runtime-on stack required"

        with mock.patch.object(run_stack_test, "_require_server_runtime_on", return_value=expected_reason):
            reason = run_stack_test._infer_prerequisite_reason(
                Path("tests/python/verify_plugin_script_matrix.py"),
                ["--scenario", "runtime-on-acceptance"],
            )

        self.assertEqual(expected_reason, reason)


if __name__ == "__main__":
    unittest.main()
