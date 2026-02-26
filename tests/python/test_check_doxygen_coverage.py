import importlib.util
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


def _load_checker_module():
    repo_root = Path(__file__).resolve().parents[2]
    module_path = repo_root / "tools" / "check_doxygen_coverage.py"

    spec = importlib.util.spec_from_file_location("check_doxygen_coverage", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module spec: {module_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class CheckDoxygenCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = _load_checker_module()

    def _write(self, root: Path, rel: str, content: str) -> Path:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content).lstrip("\n"), encoding="utf-8")
        return path

    def test_multiline_public_method_missing_return_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = self._write(
                root,
                "core/include/demo/sample.hpp",
                """
                #pragma once

                /** @brief 멀티라인 함수 선언 테스트 클래스 */
                class Sample {
                public:
                    /**
                     * @brief 합계를 계산합니다.
                     * @param left 좌항
                     * @param right 우항
                     */
                    int sum(
                        int left,
                        int right
                    ) const;
                };
                """,
            )

            issues = self.mod.check_header_file(header, root)
            self.assertEqual(1, len(issues))
            self.assertEqual("sum", issues[0].symbol)
            self.assertEqual(("@return",), issues[0].missing_tags)

    def test_public_method_without_doxygen_block_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = self._write(
                root,
                "core/include/demo/no_block.hpp",
                """
                #pragma once

                /** @brief 샘플 클래스 */
                class Sample {
                public:
                    int value(int x) const;
                };
                """,
            )

            issues = self.mod.check_header_file(header, root)
            self.assertEqual([], issues)

    def test_generated_headers_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write(
                root,
                "core/include/demo/generated.hpp",
                """
                // 자동 생성 파일: tools/gen_something.py
                #pragma once

                class Generated {
                public:
                    int f(int x) const;
                };
                """,
            )

            issues = self.mod.collect_header_issues(root)
            self.assertEqual([], issues)

    def test_deleted_special_members_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = self._write(
                root,
                "core/include/demo/deleted_members.hpp",
                """
                #pragma once

                /** @brief 삭제 멤버 테스트 클래스 */
                class Sample {
                public:
                    Sample(const Sample&) = delete;
                    Sample& operator=(const Sample&) = delete;
                };
                """,
            )

            issues = self.mod.check_header_file(header, root)
            self.assertEqual([], issues)

    def test_std_function_field_is_not_treated_as_method(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            header = self._write(
                root,
                "core/include/demo/function_field.hpp",
                """
                #pragma once
                #include <functional>

                /** @brief 함수 필드 테스트 구조체 */
                struct HandlerBucket {
                    std::function<void()> on_event;
                };
                """,
            )

            issues = self.mod.check_header_file(header, root)
            self.assertEqual([], issues)


if __name__ == "__main__":
    unittest.main()
