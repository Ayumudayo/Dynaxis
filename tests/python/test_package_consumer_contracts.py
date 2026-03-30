import unittest
from pathlib import Path


class PackageConsumerContractsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[2]
        cls.redis_consumer_cmake = (
            cls.repo_root / "tests" / "package" / "factory_redis_consumer" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.tests_cmake = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((cls.repo_root / "tests").rglob("CMakeLists.txt"))
        )
        cls.docs_build = (cls.repo_root / "docs" / "build.md").read_text(encoding="utf-8")
        cls.docs_storage = (
            cls.repo_root / "docs" / "core-api" / "storage.md"
        ).read_text(encoding="utf-8")

    def test_factory_redis_consumer_prefers_canonical_package_and_keeps_legacy_fallback(self):
        self.assertIn("find_package(infra_redis_factory CONFIG QUIET)", self.redis_consumer_cmake)
        self.assertIn("find_package(server_storage_redis_factory CONFIG REQUIRED)", self.redis_consumer_cmake)
        self.assertIn("infra_redis_factory::infra_redis_factory", self.redis_consumer_cmake)
        self.assertIn("server_storage_redis_factory::server_storage_redis_factory", self.redis_consumer_cmake)

    def test_contract_tests_keep_factory_redis_installed_consumer_lane(self):
        self.assertIn("NAME FactoryRedisInstalledPackageConsumer", self.tests_cmake)
        self.assertIn("tests/package/factory_redis_consumer", self.tests_cmake)

    def test_docs_describe_infra_redis_factory_as_canonical_surface(self):
        self.assertIn("infra_redis_factory", self.docs_build)
        self.assertIn("infra_redis_factory", self.docs_storage)
        self.assertIn("server_storage_redis_factory", self.docs_build)
        self.assertIn("server_storage_redis_factory", self.docs_storage)


if __name__ == "__main__":
    unittest.main()
