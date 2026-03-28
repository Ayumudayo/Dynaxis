import json
import unittest
from pathlib import Path


class WindowsValidationLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        presets_path = cls.repo_root / "CMakePresets.json"
        cls.presets = json.loads(presets_path.read_text(encoding="utf-8"))
        cls.configure_presets = {
            preset["name"]: preset for preset in cls.presets.get("configurePresets", [])
        }
        cls.test_presets = {
            preset["name"]: preset for preset in cls.presets.get("testPresets", [])
        }

    def _resolved_configure_preset(self, name: str) -> dict:
        preset = dict(self.configure_presets[name])
        inherited = preset.get("inherits")
        if not inherited:
            return preset

        if isinstance(inherited, str):
            inherited_names = [inherited]
        else:
            inherited_names = list(inherited)

        resolved = {}
        for inherited_name in inherited_names:
            resolved.update(self._resolved_configure_preset(inherited_name))
        resolved.update(preset)
        return resolved

    def test_windows_test_uses_isolated_release_binary_dir(self):
        windows_test = self.test_presets["windows-test"]
        configure = self._resolved_configure_preset(windows_test["configurePreset"])
        self.assertTrue(
            configure["binaryDir"].endswith("/build-windows-release"),
            msg="windows-test must use a release-only build tree so Debug stack helpers do not invalidate it",
        )

    def test_release_gate_and_default_windows_debug_tree_do_not_share_binary_dir(self):
        windows_test = self.test_presets["windows-test"]
        release_configure = self._resolved_configure_preset(windows_test["configurePreset"])
        default_windows = self._resolved_configure_preset("windows")
        self.assertNotEqual(
            release_configure["binaryDir"],
            default_windows["binaryDir"],
            msg="Debug-oriented default Windows builds must not share the release gate build tree",
        )


if __name__ == "__main__":
    unittest.main()
