import unittest
from pathlib import Path

import yaml


class CiWorkflowContractsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.workflows_dir = cls.repo_root / ".github" / "workflows"
        cls.docs_tests = cls.repo_root / "docs" / "tests.md"

    def _load_workflow(self, filename: str) -> dict:
        path = self.workflows_dir / filename
        return yaml.safe_load(path.read_text(encoding="utf-8"))

    def _paths(self, workflow: dict, event: str) -> list[str]:
        return self._on(workflow)[event]["paths"]

    def _job(self, workflow: dict, job_id: str) -> dict:
        return workflow["jobs"][job_id]

    def _step_names(self, workflow: dict, job_id: str) -> list[str]:
        return [step.get("name", "") for step in self._job(workflow, job_id)["steps"]]

    def _on(self, workflow: dict) -> dict:
        if "on" in workflow:
            return workflow["on"]
        if True in workflow:
            return workflow[True]
        raise KeyError("workflow trigger map not found")

    def _job_names(self, workflow: dict) -> set[str]:
        return {
            job.get("name", job_id)
            for job_id, job in workflow["jobs"].items()
        }

    def test_workflow_display_names_follow_the_new_taxonomy(self):
        expected_names = {
            "ci.yml": "Baseline Checks",
            "ci-api-governance.yml": "Core API Checks",
            "ci-stack.yml": "Stack Integration",
            "ci-extensibility.yml": "Extensibility Integration",
            "ci-hardening.yml": "Reliability Checks",
            "ci-prewarm.yml": "Cache Prep",
            "conan2-poc.yml": "Conan Cache Strategy Probe",
            "windows-sccache-poc.yml": "Compiler Cache Timing Probe",
            "factory-package-publish.yml": "Factory Package Release",
        }

        for filename, expected_name in expected_names.items():
            with self.subTest(filename=filename):
                workflow = self._load_workflow(filename)
                self.assertEqual(expected_name, workflow["name"])

    def test_baseline_checks_keep_stage1_required_context_and_add_markdown_validation(self):
        workflow = self._load_workflow("ci.yml")
        job = self._job(workflow, "windows-fast-tests")

        self.assertEqual("Baseline Checks", workflow["name"])
        self.assertIn("merge_group", self._on(workflow))
        self.assertEqual(
            "windows-fast-tests",
            job.get("name", "windows-fast-tests"),
        )
        self.assertIn("Validate Markdown Links", self._step_names(workflow, "windows-fast-tests"))

    def test_core_api_checks_cover_direct_script_dependencies_and_drop_fuzz_build_overlap(self):
        workflow = self._load_workflow("ci-api-governance.yml")

        self.assertEqual("Core API Checks", workflow["name"])
        self.assertNotIn("merge_group", self._on(workflow))
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                paths = self._paths(workflow, event)
                self.assertIn("scripts/build.ps1", paths)
                self.assertIn("scripts/setup_conan.ps1", paths)
                self.assertIn(".github/workflows/**", paths)

        self.assertEqual(
            "Core API Governance and Consumer Tests (Windows)",
            self._job(workflow, "core-api-consumer-windows")["name"],
        )
        linux_job = self._job(workflow, "core-api-consumer-linux")
        self.assertEqual(
            "Core API Governance and Consumer Tests (Linux)",
            linux_job["name"],
        )
        run_scripts = "\n".join(
            step["run"] for step in linux_job["steps"] if "run" in step
        )
        self.assertNotIn("protocol_fuzz_harness", run_scripts)

    def test_stack_integration_has_new_linux_checks_and_paths(self):
        workflow = self._load_workflow("ci-stack.yml")

        self.assertEqual("Stack Integration", workflow["name"])
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                paths = self._paths(workflow, event)
                self.assertIn(".github/workflows/**", paths)
                self.assertIn("scripts/run_linux_installed_consumer.ps1", paths)
                self.assertIn("tests/python/verify_admin_read_only.py", paths)
                self.assertIn("scripts/smoke_wb.sh", paths)

        self.assertEqual(
            "Stack Runtime Integration (Linux)",
            self._job(workflow, "linux-docker-stack")["name"],
        )
        job_names = self._job_names(workflow)
        self.assertIn("Admin Read-Only Check (Linux)", job_names)
        self.assertIn("Write-Behind Integration Check (Linux)", job_names)
        admin_job_steps = self._job(workflow, "admin-read-only-check-linux")["steps"]
        admin_build_step = next(
            step for step in admin_job_steps if step.get("name") == "Build Admin Runtime Image"
        )
        self.assertIn("docker build", admin_build_step["run"])
        self.assertIn("--target admin-runtime", admin_build_step["run"])

        smoke_script = self.repo_root / "scripts" / "smoke_wb.sh"
        self.assertTrue(smoke_script.is_file())
        text = smoke_script.read_text(encoding="utf-8")
        self.assertIn("redis-cli", text)
        self.assertIn("session_events", text)
        self.assertIn("psql", text)

    def test_extensibility_integration_paths_cover_gateway_and_workflow_edits(self):
        workflow = self._load_workflow("ci-extensibility.yml")

        self.assertEqual("Extensibility Integration", workflow["name"])
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                paths = self._paths(workflow, event)
                self.assertIn(".github/workflows/**", paths)
                self.assertIn("gateway/**", paths)

        self.assertEqual(
            "Plugin and Lua Runtime Integration (Linux)",
            self._job(workflow, "linux-extensibility")["name"],
        )

    def test_reliability_checks_are_push_schedule_and_manual_only(self):
        workflow = self._load_workflow("ci-hardening.yml")

        self.assertEqual("Reliability Checks", workflow["name"])
        self.assertNotIn("merge_group", self._on(workflow))
        self.assertIn("schedule", self._on(workflow))
        self.assertIn("workflow_dispatch", self._on(workflow))
        self.assertEqual(
            "Sanitizer, Fuzz, and Soak Checks (Linux)",
            self._job(workflow, "linux-hardening")["name"],
        )

    def test_support_probe_and_release_job_names_are_reclassified(self):
        prewarm = self._load_workflow("ci-prewarm.yml")
        self.assertEqual("Cache Prep", prewarm["name"])
        self.assertEqual(
            "Conan Cache Prep (Windows)",
            self._job(prewarm, "windows-conan-prewarm")["name"],
        )
        self.assertEqual(
            "Base Image Cache Prep (Linux)",
            self._job(prewarm, "linux-base-image-prewarm")["name"],
        )

        conan_probe = self._load_workflow("conan2-poc.yml")
        self.assertEqual("Conan Cache Strategy Probe", conan_probe["name"])
        self.assertEqual(
            "Conan Cache Strategy Probe (Windows)",
            self._job(conan_probe, "windows-conan2-poc")["name"],
        )

        sccache_probe = self._load_workflow("windows-sccache-poc.yml")
        self.assertEqual("Compiler Cache Timing Probe", sccache_probe["name"])
        self.assertEqual(
            "Compiler Cache Timing Probe (Windows)",
            self._job(sccache_probe, "windows-sccache-poc")["name"],
        )

        release = self._load_workflow("factory-package-publish.yml")
        self.assertEqual("Factory Package Release", release["name"])
        self.assertEqual(
            "Factory Package Bundle Build (Windows)",
            self._job(release, "publish-factory-packages-windows")["name"],
        )

    def test_docs_tests_md_reflects_the_new_lane_taxonomy(self):
        text = self.docs_tests.read_text(encoding="utf-8")

        self.assertIn("## Validation Lanes", text)
        self.assertIn("## Operational Support Lanes", text)
        self.assertIn("## Probe Lanes", text)
        self.assertIn("## Release Lanes", text)
        self.assertIn("`windows-fast-tests`", text)
        self.assertIn("merge queue", text)
        self.assertIn("`Stack Integration`", text)
        self.assertIn("`Extensibility Integration`", text)
        self.assertIn("`Stack Runtime Integration (Linux)`", text)
        self.assertIn("`Admin Read-Only Check (Linux)`", text)
        self.assertIn("`Write-Behind Integration Check (Linux)`", text)
        self.assertIn("`Compiler Cache Timing Probe`", text)
        self.assertNotIn("`Core API Checks`는 merge queue 대상", text)
        lowered = text.lower()
        self.assertTrue(
            "manual follow-up" in lowered or "수동 후속" in text,
            "docs/tests.md should mention that settings changes are a manual follow-up",
        )


if __name__ == "__main__":
    unittest.main()
