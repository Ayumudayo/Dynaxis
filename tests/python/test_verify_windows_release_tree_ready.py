import io
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import verify_windows_release_tree_ready as release_tree_ready


class VerifyWindowsReleaseTreeReadyTests(unittest.TestCase):
    def _write(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")

    def test_reports_missing_discovered_gtest_metadata(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            build_dir = Path(tmp_dir_raw)
            include_path = build_dir / "tests" / "storage_basic_tests[1]_include.cmake"
            missing_tests_path = build_dir / "tests" / "storage_basic_tests[1]_tests.cmake"
            self._write(
                include_path,
                f"""
                if(EXISTS "{missing_tests_path.as_posix()}")
                  include("{missing_tests_path.as_posix()}")
                else()
                  add_test(storage_basic_tests_NOT_BUILT storage_basic_tests_NOT_BUILT)
                endif()
                """,
            )

            failures = release_tree_ready.collect_release_tree_failures(build_dir, "Release")

        self.assertEqual(1, len(failures))
        self.assertIn("storage_basic_tests[1]_tests.cmake", failures[0])

    def test_reports_missing_release_executable_registered_in_ctest(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            build_dir = Path(tmp_dir_raw)
            ctest_file = build_dir / "tests" / "CTestTestfile.cmake"
            expected_executable = build_dir / "tests" / "Release" / "protocol_fuzz_harness.exe"
            self._write(
                ctest_file,
                f"""
                if(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
                  add_test([=[ProtocolFuzzHarness]=] "{expected_executable.as_posix()}")
                else()
                  add_test([=[ProtocolFuzzHarness]=] NOT_AVAILABLE)
                endif()
                """,
            )

            failures = release_tree_ready.collect_release_tree_failures(build_dir, "Release")

        self.assertEqual(1, len(failures))
        self.assertIn("ProtocolFuzzHarness", failures[0])
        self.assertIn("protocol_fuzz_harness.exe", failures[0])

    def test_main_succeeds_when_release_tree_metadata_and_executables_exist(self):
        with tempfile.TemporaryDirectory() as tmp_dir_raw:
            build_dir = Path(tmp_dir_raw)
            include_path = build_dir / "tests" / "core_general_tests[1]_include.cmake"
            tests_path = build_dir / "tests" / "core_general_tests[1]_tests.cmake"
            expected_executable = build_dir / "tests" / "Release" / "core_public_api_smoke.exe"
            self._write(
                include_path,
                f"""
                if(EXISTS "{tests_path.as_posix()}")
                  include("{tests_path.as_posix()}")
                else()
                  add_test(core_general_tests_NOT_BUILT core_general_tests_NOT_BUILT)
                endif()
                """,
            )
            self._write(tests_path, "# generated gtest metadata")
            expected_executable.parent.mkdir(parents=True, exist_ok=True)
            expected_executable.write_text("", encoding="utf-8")
            self._write(
                build_dir / "tests" / "CTestTestfile.cmake",
                f"""
                if(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
                  add_test([=[CorePublicApiSmoke]=] "{expected_executable.as_posix()}")
                else()
                  add_test([=[CorePublicApiSmoke]=] NOT_AVAILABLE)
                endif()
                """,
            )

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                exit_code = release_tree_ready.main(["--build-dir", str(build_dir), "--config", "Release"])

        self.assertEqual(0, exit_code)
        self.assertIn("PASS", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
