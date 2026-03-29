import io
import unittest
from contextlib import redirect_stdout
from unittest import mock

import verify_plugin_script_matrix as plugin_script_matrix


class VerifyPluginScriptMatrixTests(unittest.TestCase):
    def test_runtime_on_acceptance_plan_matches_expected_sequence(self):
        expected = [
            (
                "runtime-toggles-on",
                [
                    str(plugin_script_matrix.RUNTIME_TOGGLE_SCRIPT),
                    "--expect-chat-hook-enabled",
                    "1",
                    "--expect-lua-enabled",
                    "1",
                ],
            ),
            ("plugin-metrics-check-only", [str(plugin_script_matrix.PLUGIN_HOT_RELOAD_SCRIPT), "--check-only"]),
            ("plugin-hot-reload", [str(plugin_script_matrix.PLUGIN_HOT_RELOAD_SCRIPT)]),
            ("plugin-v2-fallback", [str(plugin_script_matrix.PLUGIN_V2_FALLBACK_SCRIPT)]),
            ("plugin-rollback", [str(plugin_script_matrix.PLUGIN_ROLLBACK_SCRIPT)]),
            ("script-hot-reload", [str(plugin_script_matrix.SCRIPT_HOT_RELOAD_SCRIPT)]),
            ("script-fallback-switch", [str(plugin_script_matrix.SCRIPT_FALLBACK_SWITCH_SCRIPT)]),
            ("chat-hook-behavior", [str(plugin_script_matrix.CHAT_HOOK_BEHAVIOR_SCRIPT)]),
        ]

        self.assertEqual(expected, plugin_script_matrix.build_plan("runtime-on-acceptance"))

    def test_matrix_alias_uses_runtime_on_acceptance_plan(self):
        self.assertEqual(
            plugin_script_matrix.build_plan("runtime-on-acceptance"),
            plugin_script_matrix.build_plan("matrix"),
        )

    def test_main_runs_every_stage_in_order(self):
        calls: list[tuple[str, list[str]]] = []

        def fake_run_stage(name: str, command: list[str]) -> None:
            calls.append((name, command))

        stdout = io.StringIO()
        with mock.patch.object(plugin_script_matrix, "run_stage", side_effect=fake_run_stage):
            with redirect_stdout(stdout):
                exit_code = plugin_script_matrix.main([])

        self.assertEqual(0, exit_code)
        self.assertEqual(plugin_script_matrix.build_plan("runtime-on-acceptance"), calls)
        self.assertIn("PASS runtime-on-acceptance", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
