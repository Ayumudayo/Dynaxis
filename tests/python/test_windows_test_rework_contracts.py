import unittest
from pathlib import Path


class WindowsTestReworkContractsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.tests_cmake = (cls.repo_root / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.ci_workflow = (cls.repo_root / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        cls.docs_build = (cls.repo_root / "docs" / "build.md").read_text(encoding="utf-8")
        cls.docs_tests = (cls.repo_root / "docs" / "tests.md").read_text(encoding="utf-8")

    def test_ctest_registers_plugin_script_acceptance_suite(self):
        self.assertIn("NAME StackPythonPluginScriptAcceptance", self.tests_cmake)
        self.assertIn("verify_plugin_script_matrix.py", self.tests_cmake)

    def test_ctest_registers_windows_release_tree_preflight(self):
        self.assertIn("NAME WindowsReleaseTreeReady", self.tests_cmake)
        self.assertIn("verify_windows_release_tree_ready.py", self.tests_cmake)

    def test_duplicate_leaf_stack_ctest_entries_are_removed(self):
        removed_entries = (
            "NAME StackPythonVerifyPluginMetrics",
            "NAME StackPythonVerifyRuntimeTogglesOn",
            "NAME StackPythonHotReloadPluginV2",
            "NAME StackPythonVerifyPluginV2Fallback",
            "NAME StackPythonRollbackPluginV1",
            "NAME StackPythonHotReloadLuaScript",
            "NAME StackPythonVerifyScriptFallbackSwitch",
            "NAME StackPythonChatHookBehavior",
            "NAME StackPythonVerifyFpsStateTransport",
            "NAME StackPythonVerifyFpsRudpTransport",
            "NAME StackPythonVerifyFpsRudpTransportMatrix",
            "NAME StackPythonVerifyFpsNetemRehearsal",
            "NAME StackPythonVerifyWorldDrainProgress",
            "NAME StackPythonVerifyWorldDrainTransferClosure",
            "NAME StackPythonVerifyWorldDrainMigrationClosure",
            "NAME StackPythonVerifyWorldOwnerTransferCommit",
            "NAME StackPythonVerifyWorldMigrationHandoff",
            "NAME StackPythonVerifyWorldMigrationTargetRoomHandoff",
        )
        for entry in removed_entries:
            with self.subTest(entry=entry):
                self.assertNotIn(entry, self.tests_cmake)

    def test_ci_workflow_runs_release_tree_preflight_before_full_windows_ctest(self):
        self.assertIn(
            "ctest --preset windows-test -R WindowsReleaseTreeReady --output-on-failure --no-tests=error",
            self.ci_workflow,
        )
        self.assertIn(
            "ctest --preset windows-test --parallel 8 --output-on-failure",
            self.ci_workflow,
        )

    def test_build_and_tests_docs_reference_windows_release_tree_preflight(self):
        self.assertIn("WindowsReleaseTreeReady", self.docs_build)
        self.assertIn("WindowsReleaseTreeReady", self.docs_tests)


if __name__ == "__main__":
    unittest.main()
